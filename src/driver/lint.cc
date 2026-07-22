#include "driver/driver.h"

namespace clice::driver {

namespace {

struct LintOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;
};

auto make_command() {
    return kota::deco::cli::command<LintOptions>("clice lint [OPTIONS]");
}

}  // namespace

void add_lint(kota::deco::cli::SubCommander& root, int& exit_code) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code](LintOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           std::println(
               stderr,
               "clice lint is not implemented yet: it will run clang-tidy across the workspace with cross-TU caching.");
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "lint", .description = "Lint C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
