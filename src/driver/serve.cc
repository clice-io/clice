module;

#include "modules/prelude.h"

module clice;

import :driver.driver;
import :server.master_server;

namespace clice::driver {

namespace {

struct ServeOptions {
    kota::deco::decl::HelpOption help;
    ServerOptions server;
    LogLevelOption log;
};

CommandHandler build(const char* self_path) {
    auto cmd = kota::deco::cli::command<ServeOptions>("clice serve [OPTIONS]");
    cmd.match_all([self_path](ServeOptions opts) {
        opts.log.apply();
        return run_serve_mode(opts.server, self_path);
    });
    return cmd;
}

[[maybe_unused]] const bool registered =
    register_command(Command::serve, {.description = "Start LSP server", .build = build});

}  // namespace

}  // namespace clice::driver
