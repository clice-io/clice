module clice;

import :driver.driver;
import :server.master;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Serve, description = "Start LSP server");

struct Serve {
    constexpr static std::string_view usage_line = "clice serve [OPTIONS]";

    KANNOTATE(&help, cli::help)
    bool help = false;

    ServerOptions server;
    ConfigurationOption configuration;
    LogLevelOption log{.log_level = LogLevel::Info};
};

/// The server logs to stderr as a client may read it (run_serve_mode).
int run_serve(Serve& serve) {
    if(print_help(serve)) {
        return 0;
    }
    serve.log.apply();
    return run_serve_mode(serve.server, serve.configuration.configuration);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(run_serve);

}  // namespace

}  // namespace clice::driver
