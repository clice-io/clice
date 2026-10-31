module clice;

import :driver.driver;
import :server.master;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;
    ServerOptions server;
    ConfigurationOption configuration;
    LogLevelOption log{.log_level = LogLevel::Info};
};

CommandHandler build() {
    auto cmd = kota::deco::cli::command<Options>("clice serve [OPTIONS]");
    cmd.match_all([](Options opts) {
        opts.log.apply();
        return run_serve_mode(opts.server, opts.configuration.tag());
    });
    return cmd;
}

[[maybe_unused]] const bool registered =
    register_command(Command::serve, {.description = "Start LSP server", .build = build});

}  // namespace

}  // namespace clice::driver
