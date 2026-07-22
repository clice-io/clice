#include "driver/driver.h"
#include "server/transport/agentic.h"

namespace clice::driver {

namespace {

auto make_command() {
    return kota::deco::cli::command<QueryOptions>("clice query [OPTIONS]");
}

}  // namespace

void add_query(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](QueryOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           auto port = opts.port.value_or(0);
           if(port <= 0 || port > 65535) {
               LOG_ERROR("--port must be between 1 and 65535");
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           exit_code = run_agentic_mode(opts);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "query", .description = "Query symbol information from a running server"},
             std::move(cmd));
}

}  // namespace clice::driver
