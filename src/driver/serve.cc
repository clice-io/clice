#include "driver/driver.h"
#include "server/master_server.h"

namespace clice::driver {

namespace {

struct ServeOptions {
    kota::deco::decl::HelpOption help;
    ServerOptions server;
    LogLevelOption log;
};

}  // anonymous namespace

void add_serve(kota::deco::cli::SubCommander& root, const char* self_path) {
    auto cmd = kota::deco::cli::command<ServeOptions>("clice serve [OPTIONS]");
    cmd.match_all([self_path](ServeOptions opts) {
        opts.log.apply();
        return run_serve_mode(opts.server, self_path);
    });

    root.add({.name = "serve", .description = "Start LSP server"}, std::move(cmd));
}

}  // namespace clice::driver
