module;

#include "modules/prelude.h"

module clice;

import :driver.driver;
import :server.master_server;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;
    ServerOptions server;
    ConfigurationOption configuration;
    LogLevelOption log{.log_level = LogLevel::Info};
};

CommandHandler build(const char* self_path) {
    auto cmd = kota::deco::cli::command<Options>("clice serve [OPTIONS]");
    cmd.match_all([self_path](Options opts) {
        opts.log.apply();
        return run_serve_mode(opts.server, opts.configuration.tag(), self_path);
    });
    return cmd;
}

[[maybe_unused]] const bool registered =
    register_command(Command::serve, {.description = "Start LSP server", .build = build});

}  // namespace

}  // namespace clice::driver
