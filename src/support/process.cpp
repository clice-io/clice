module;

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <signal.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <unistd.h>
#endif

module clice;

import :support.environment;
import :support.logging;
import :support.process;

namespace clice {

async::task<std::expected<ProgramOutput, std::string>> execute(std::vector<std::string> arguments,
                                                               std::string cwd,
                                                               std::string path) {
    auto file = arguments[0];
    LOG_INFO("Execute command: {}", file);

    arguments.erase(arguments.begin());
    // Keeps the output callers parse unlocalized. Not LANG alone: GCC
    // translates its `-v` report under LC_ALL or LC_MESSAGES, which win over
    // LANG.
    std::vector<sys::EnvChange> env_changes = {
        {.name = "LC_ALL", .value = "C"}
    };
    if(!path.empty()) {
        env_changes.push_back({.name = "PATH", .value = std::move(path)});
    }
    auto captured = co_await async::process::capture({
        .program = file,
        .args = std::move(arguments),
        .env_changes = std::move(env_changes),
        .cwd = std::move(cwd),
        .creation = {.exit_with_parent = can_exit_with_parent},
    });
    if(!captured) {
        co_return std::unexpected(
            std::format("Failed to run {}: {}", file, captured.error().message()));
    }
    ProgramOutput output{.out = std::move(captured->stdout_data),
                         .err = std::move(captured->stderr_data)};
    if(!captured->status.success()) {
        output.failure =
            std::format("Process {} ended with {}", file, captured->status.to_string());
    }
    co_return output;
}

extern "C" {

    // Set by the allocator of the Windows and macOS executables
    // (src/mimalloc.cc), which no other program links: its own collect.
    void (*clice_allocator_collect)() = nullptr;
}

void release_free_memory() {
    // Under ASan glibc's allocator holds nothing: malloc_trim would only
    // initialize it, which glibc does without a lock, so two threads trimming
    // at once corrupt it (a crash then, or an abort at thread exit).
#if defined(__GLIBC__) && !defined(__SANITIZE_ADDRESS__)
    malloc_trim(0);
#endif
    if(clice_allocator_collect) {
        clice_allocator_collect();
    }
}

/// release_free_memory on every pool thread, then on this one.
static async::task<> collect_everywhere() {
    if(!clice_allocator_collect) {
        co_await async::queue([] { release_free_memory(); });
        co_return;
    }
    auto threads = thread_pool_size();
    std::mutex mutex;
    std::condition_variable arrived;
    std::size_t here = 0;
    // Once one collect has waited in vain, the threads still busy will not
    // come: the later collects, queued behind them, run without waiting.
    bool waited = false;
    std::vector<async::task<>> collects;
    for(std::size_t i = 0; i < threads; i += 1) {
        collects.push_back(async::queue([&] {
            {
                std::unique_lock lock(mutex);
                here += 1;
                arrived.notify_all();
                if(!waited && !arrived.wait_for(lock, chrono::milliseconds(50), [&] {
                       return here == threads;
                   })) {
                    waited = true;
                }
            }
            release_free_memory();
        }));
    }
    co_await async::when_all(std::move(collects));
    release_free_memory();
}

async::task<> release_free_memory_everywhere() {
    // Every call comes from the loop thread.
    static bool running = false;
    static bool again = false;
    if(running) {
        again = true;
        co_return;
    }
    running = true;
    auto done = llvm::make_scope_exit([] { running = false; });
    do {
        again = false;
        co_await collect_everywhere();
    } while(again);
}

async::task<> watch_termination(int signum, bool& requested, std::function<void()> stop) {
    // Runs after the watcher closes, which restores the default action: a
    // repeated SIGTERM or SIGHUP would then kill a process that is only left
    // to report and exit.
    auto keep_ignoring = llvm::make_scope_exit([&] {
        if(requested && signum != SIGINT) {
            std::signal(signum, SIG_IGN);
        }
    });
    auto watcher = async::signal::create();
    if(!watcher || watcher->start(signum).has_error()) {
        co_return;
    }
    while(true) {
        co_await watcher->wait();
        if(!requested) {
            requested = true;
            stop();
        } else if(signum == SIGINT) {
            std::_Exit(130);
        }
    }
}

#ifdef __APPLE__

void exit_with_parent() {
    auto parent = ::getppid();
    auto queue = ::kqueue();
    struct kevent change;
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    // launchd adopts an orphan: a parent of 1 exited before this ran.
    if(parent == 1 || queue == -1 || ::kevent(queue, &change, 1, nullptr, 0, nullptr) == -1 ||
       ::getppid() != parent) {
        std::_Exit(1);
    }
    std::thread([queue] {
        struct kevent event;
        while(::kevent(queue, nullptr, 0, &event, 1, nullptr) == -1 && errno == EINTR) {}
        std::_Exit(1);
    }).detach();
}

#endif

}  // namespace clice
