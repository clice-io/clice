module;

#include "modules/prelude.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <unistd.h>
#else
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

#include "support/logging.macros.h"

module clice;

import :support.logging;
import :support.process;

namespace clice {

kota::task<std::expected<std::string, std::string>> execute(std::vector<std::string> arguments,
                                                            bool capture_stdout,
                                                            std::string cwd) {
    auto file = arguments[0];
    LOG_INFO("Execute command: {}", file);

    auto captured = co_await kota::process::capture({
        .file = file,
        .args = std::move(arguments),
        // LANG=C keeps the driver output, which callers parse, unlocalized.
        .env_changes = {{.name = "LANG", .value = "C"}},
        .cwd = std::move(cwd),
    });
    if(!captured) {
        co_return std::unexpected(
            std::format("Failed to run {}: {}", file, captured.error().message()));
    }
    if(!captured->status.success()) {
        co_return std::unexpected(
            std::format("Process {} ended with {}", file, captured->status.to_string()));
    }

    co_return capture_stdout ? std::move(captured->stdout_data) : std::move(captured->stderr_data);
}

void release_free_memory() {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
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

void exit_with_parent(std::uint32_t parent) {
#ifdef _WIN32
    auto process = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if(!process) {
        std::_Exit(1);
    }
    std::thread([process] {
        WaitForSingleObject(process, INFINITE);
        std::_Exit(1);
    }).detach();
#elif defined(__APPLE__)
    auto queue = ::kqueue();
    struct kevent change;
    EV_SET(&change, parent, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if(queue == -1 || ::kevent(queue, &change, 1, nullptr, 0, nullptr) == -1 ||
       ::getppid() != static_cast<pid_t>(parent)) {
        std::_Exit(1);
    }
    std::thread([queue] {
        struct kevent event;
        while(::kevent(queue, nullptr, 0, &event, 1, nullptr) == -1 && errno == EINTR) {}
        std::_Exit(1);
    }).detach();
#else
    // The death signal follows the thread that spawned this process; the
    // check after arming it catches a parent that died before.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if(::getppid() != static_cast<pid_t>(parent)) {
        std::_Exit(1);
    }
#endif
}

}  // namespace clice
