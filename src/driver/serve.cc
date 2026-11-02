module clice;

import :driver.driver;
import :server.master;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Serve, description = "Start LSP server");
struct Serve {
    ServerOptions server;
    ConfigurationOption build;
    LogLevelOption log{.log_level = LogLevel::Info};
};

/// Not through dispatch: the server installs a stderr logger of its own,
/// which a client that does not drain stderr cannot block (run_serve_mode).
int run_serve(Serve& serve) {
    serve.log.apply();
    return run_serve_mode(serve.server, serve.build.configuration);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(run_serve);

}  // namespace

}  // namespace clice::driver
