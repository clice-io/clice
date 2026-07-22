#include "driver/driver.h"

namespace clice::driver {

namespace {

struct DocOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;
};

auto make_command() {
    return kota::deco::cli::command<DocOptions>("clice doc [OPTIONS]");
}

}  // namespace

void add_doc(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](DocOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           std::println(
               stderr,
               "clice doc is not implemented yet: it will generate documentation data for the workspace.");
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "doc", .description = "Generate documentation data"}, std::move(cmd));
}

}  // namespace clice::driver
