module;

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

module clice;

import :support.logging;
import :support.process;

namespace clice {

async::task<> in_parts(std::size_t size,
                       llvm::function_ref<void(std::size_t, std::size_t, std::size_t)> work) {
    auto count = parts_of(size);
    async::task_group<> parts;
    for(std::size_t part = 0; part < count; part += 1) {
        parts.spawn([](llvm::function_ref<void(std::size_t, std::size_t, std::size_t)> work,
                       std::size_t part,
                       std::size_t first,
                       std::size_t end) -> async::task<> {
            co_await async::queue([&] { work(part, first, end); });
        }(work, part, size * part / count, size * (part + 1) / count));
    }
    co_await parts.join();
}

std::expected<std::string, std::string> temp_root() {
    std::string dir;
    if(auto error = sys::temp_dir(dir)) {
        return std::unexpected(std::format("no temporary directory: {}", error.message()));
    }
#ifdef _WIN32
    fs::path::push(dir, "clice");
#else
    fs::path::push(dir, std::format("clice-{}", getuid()));
#endif
    auto unusable = [&](std::error_code error) {
        return std::unexpected(std::format("{} cannot hold clice's temporary files: {}{}",
                                           dir,
                                           error.message(),
#ifdef _WIN32
                                           ""
#else
                                           "; another user may own it: point TMPDIR at a "
                                           "directory of your own"
#endif
                                           ));
    };
    if(auto made = fs::create_directory(dir); !made) {
        return unusable(made.error());
    }
#ifndef _WIN32
    // Its own, a directory: not another user's, which root may still chmod,
    // nor a link planted at its name.
    struct ::stat entry{};
    if(::lstat(dir.c_str(), &entry) != 0) {
        return unusable(std::error_code(errno, std::generic_category()));
    }
    if(!S_ISDIR(entry.st_mode) || entry.st_uid != getuid()) {
        return unusable(std::make_error_code(std::errc::permission_denied));
    }
    if(auto error = fs::set_permissions(dir, 0700)) {
        return unusable(error);
    }
#endif
    return dir;
}

void remove_left_by_exited(llvm::StringRef parent) {
    auto entries = fs::Directory::open(parent);
    if(!entries) {
        return;
    }
    auto self = sys::pid();
    for(auto& entry: *entries) {
        if(!entry) {
            break;
        }
        int pid = 0;
        if(llvm::StringRef(entry->name).getAsInteger(10, pid) || pid == self ||
           sys::process_alive(pid)) {
            continue;
        }
        std::string dead = parent.str();
        fs::path::push(dead, entry->name);
        fs::remove_all(dead);
        LOG_DEBUG("Removed {}, left by a clice that exited", dead);
    }
}

async::task<std::expected<ProgramOutput, std::string>> execute(std::vector<std::string> arguments,
                                                               std::string cwd,
                                                               std::string path) {
    auto file = arguments[0];
    LOG_DEBUG("Running {}", file);

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
        .creation = {.process_group = true, .exit_with_parent = can_exit_with_parent},
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

    // Set by the executables' allocator (src/mimalloc.cc), which no other
    // program links: its own collect.
    void (*clice_allocator_collect)() = nullptr;
}

void release_free_memory() {
    if(!clice_allocator_collect) {
        sys::release_free_heap();
        return;
    }
    clice_allocator_collect();
#ifndef __linux__
    // The system's heap still serves part of the process: the UCRT's own
    // blocks on Windows, the malloc zones other than mimalloc's on macOS. On
    // Linux mimalloc has taken the C library's malloc over, which then holds
    // nothing, and glibc's malloc_trim would set it up on its first call,
    // which the pool threads release_free_memory_everywhere runs this on
    // would race.
    sys::release_free_heap();
#endif
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
        if(!clice_allocator_collect) {
            co_await async::queue([] { release_free_memory(); });
            continue;
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
    } while(again);
}

async::task<> watch_termination(int signum, bool& requested, std::function<void()> stop) {
    auto watcher = async::signal::create();
    if(!watcher || watcher->start(signum).has_error()) {
        co_return;
    }
    while(true) {
        co_await watcher->wait();
        if(!requested) {
            requested = true;
            // Once the watcher closes, a repeated SIGTERM or SIGHUP must not
            // kill a process that is only left to report and exit.
            if(signum != SIGINT) {
                watcher->start(signum, async::signal::leave::ignored);
            }
            stop();
        } else if(signum == SIGINT) {
            std::_Exit(130);
        }
    }
}

}  // namespace clice
