module;

#include "support/prelude.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <signal.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "support/logging.macros.h"

module clice;

import :support.logging;
import :support.process;

namespace clice {

kota::task<std::expected<ProgramOutput, std::string>> execute(std::vector<std::string> arguments,
                                                              std::string cwd) {
    auto file = arguments[0];
    LOG_INFO("Execute command: {}", file);

    arguments.erase(arguments.begin());
    auto captured = co_await kota::process::capture({
        .program = file,
        .args = std::move(arguments),
        // Keeps the output callers parse unlocalized. Not LANG alone: GCC
        // translates its `-v` report under LC_ALL or LC_MESSAGES, which win
        // over LANG.
        .env_changes = {{.name = "LC_ALL", .value = "C"}},
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
    // (src/mimalloc.cc), which no other program links: its own collect, and
    // on Windows the switch to purging.
    void (*clice_allocator_collect)() = nullptr;
    void (*clice_allocator_purge)() = nullptr;
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

void purge_freed_memory() {
    if(clice_allocator_purge) {
        clice_allocator_purge();
    }
}

void lower_thread_priority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#else
    // Linux keeps a nice value per thread. Not SCHED_IDLE: under a machine
    // kept busy by the user's own build it starves a TU past the build
    // deadline, which then blames the file for a hang.
    [[maybe_unused]] auto niceness = ::nice(10);
#endif
}

kota::task<> watch_termination(int signum, bool& requested, std::function<void()> stop) {
    // Runs after the watcher closes, which restores the default action: a
    // repeated SIGTERM or SIGHUP would then kill a process that is only left
    // to report and exit.
    auto keep_ignoring = llvm::make_scope_exit([&] {
        if(requested && signum != SIGINT) {
            std::signal(signum, SIG_IGN);
        }
    });
    auto watcher = kota::signal::create();
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

bool process_alive(std::uint32_t pid) {
#ifdef _WIN32
    auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if(!process) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
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

bool is_pipe_or_socket(int fd) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    return handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) == FILE_TYPE_PIPE;
#else
    struct stat st = {};
    return ::fstat(fd, &st) == 0 && (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode));
#endif
}

bool write_console(int fd, llvm::StringRef text) {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) &&
           !sys::write_console(fd, text);
#else
    return false;
#endif
}

}  // namespace clice
