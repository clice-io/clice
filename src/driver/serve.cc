#include "driver/driver.h"
#include "server/transport/master_server.h"

namespace clice::driver {

namespace {

auto make_command() {
    return kota::deco::cli::command<ServerOptions>("clice serve [OPTIONS]");
}

}  // namespace

void add_serve(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](ServerOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           exit_code = run_serve_mode(opts, self_path);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "serve", .description = "Start LSP server"}, std::move(cmd));
}

}  // namespace clice::driver
