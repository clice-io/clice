module;

#include <cstdio>  // std floor: stderr is a macro, so import stdlib cannot supply it

module clice.driver;

import stdlib;
import kota;
import clice.server;
import clice.support;

namespace clice::driver {

bool apply_log_level(const std::string& level_str) {
    auto level = logging::parse_level(level_str);
    if(!level) {
        std::println(stderr,
                     "unknown log level '{}', valid: trace, debug, info, warn, error, off",
                     level_str);
        return false;
    }
    logging::options.level = *level;
    return true;
}

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
