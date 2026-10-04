#include "support/process.h"

#include <format>
#include <utility>

#include "support/logging.h"

#include "llvm/ADT/StringRef.h"

#ifndef _WIN32
extern char** environ;
#endif

namespace clice {

namespace {

#ifndef _WIN32
/// Process environment with LANG pinned to C, so driver output is not localized.
/// On Windows the env is left empty so the child inherits the parent's
/// environment, which MSVC and clang rely on to locate the standard library.
const std::vector<std::string>& process_env() {
    const static auto env = [] {
        std::vector<std::string> result;
        if(environ) {
            for(char** e = environ; *e; ++e) {
                if(!llvm::StringRef(*e).starts_with("LANG="))
                    result.emplace_back(*e);
            }
        }
        result.emplace_back("LANG=C");
        return result;
    }();
    return env;
}
#endif

kota::task<std::string> drain_pipe(kota::pipe p) {
    std::string buf;
    while(true) {
        auto result = co_await p.read();
        if(!result.has_value())
            break;
        auto& chunk = result.value();
        if(chunk.empty())
            break;
        buf += chunk;
    }
    co_return buf;
}

}  // namespace

kota::task<std::expected<std::string, std::string>> execute(std::vector<std::string> arguments,
                                                            bool capture_stdout,
                                                            std::string cwd) {
    kota::process::options opts;
    opts.file = arguments[0];
    opts.args = std::move(arguments);
    opts.cwd = std::move(cwd);
#ifndef _WIN32
    opts.env = process_env();
#endif
    opts.streams = {
        kota::process::stdio::ignore(),
        kota::process::stdio::pipe(false, true),
        kota::process::stdio::pipe(false, true),
    };

    LOG_INFO("Execute command: {}", opts.file);

    auto spawn = kota::process::spawn(opts);
    if(!spawn.has_value()) {
        co_return std::unexpected(
            std::format("Failed to spawn {}: {}", opts.file, spawn.error().message()));
    }
    auto& s = *spawn;

    // Drain both pipes concurrently with process exit: a child blocking on a
    // full pipe would otherwise deadlock against our wait().
    auto [stdout_data, stderr_data] = co_await kota::when_all(drain_pipe(std::move(s.stdout_pipe)),
                                                              drain_pipe(std::move(s.stderr_pipe)));

    auto exit_result = co_await s.proc.wait();
    if(!exit_result.has_value()) {
        co_return std::unexpected(
            std::format("Process wait failed: {}", exit_result.error().message()));
    }

    auto& exit = *exit_result;
    if(exit.status != 0) {
        co_return std::unexpected(
            std::format("Process {} exited with code {}", opts.file, exit.status));
    }

    co_return capture_stdout ? std::move(stdout_data) : std::move(stderr_data);
}

}  // namespace clice
