module;

#include "modules/prelude.h"

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

}  // namespace clice
