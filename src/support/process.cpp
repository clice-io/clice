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
#else
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

}  // namespace clice
