#include "driver/driver.h"

namespace clice::driver {

namespace {

struct FormatOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;
};

auto make_command() {
    return kota::deco::cli::command<FormatOptions>("clice format [OPTIONS]");
}

}  // namespace

void add_format(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](FormatOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           std::println(stderr,
                        "clice format is not implemented yet: it will format C++ source files.");
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "format", .description = "Format C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
