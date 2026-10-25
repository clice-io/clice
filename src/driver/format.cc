module;

#include "support/prelude.h"

module clice;

import :driver.driver;
import :sched.batch;
import :vfs.path;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;

    DecoInput(meta_var = "<PATH>",
              help =
                  "Files to format, or directories to format the build's own files under; "
                  "relative to the workspace (default: the build's own files)",
              required = false)
    <std::vector<std::string>> paths;

    DecoFlag(names = {"--check"},
             help = "Report the files that need formatting instead of rewriting them",
             required = false)
    check;

    WorkspaceOptions where;
    JobsOption jobs;
    LogLevelOption log;
};

int run_format(BatchFormatOptions options) {
    auto result = run_batch_format(options);
    write_output(stderr, result.output);
    if(result.exit_code == 2) {
        driver::println(stderr, "{}", result.error);
        return 2;
    }
    if(result.files == 0) {
        driver::println("No files to format under {}.", options.root);
        return 0;
    }
    if(!options.check) {
        driver::println("Formatted {} file{} in {:.1f}s.",
                        result.files,
                        plural_s(result.files),
                        result.seconds);
    } else if(result.unformatted.empty()) {
        driver::println("Checked {} file{} in {:.1f}s: all formatted.",
                        result.files,
                        plural_s(result.files),
                        result.seconds);
    } else {
        driver::println("Checked {} file{} in {:.1f}s: {} need{} formatting.",
                        result.files,
                        plural_s(result.files),
                        result.seconds,
                        result.unformatted.size(),
                        result.unformatted.size() == 1 ? "s" : "");
    }
    return result.exit_code;
}

CommandHandler build() {
    return operation<Options>("clice format [<PATH>...] [OPTIONS]",
                              "format",
                              Errors::Text,
                              [](const Options& opts) {
                                  BatchFormatOptions options{
                                      .root = CanonicalPath(opts.where.root()),
                                      .configuration = opts.where.configuration.tag(),
                                      .jobs = opts.jobs.jobs.value_or(0),
                                      .check = static_cast<bool>(opts.check),
                                  };
                                  for(auto& argument:
                                      opts.paths.value_or(std::vector<std::string>{})) {
                                      options.paths.emplace_back(argument, Spelling(options.root));
                                  }
                                  return run_format(std::move(options));
                              });
}

[[maybe_unused]] const bool registered =
    register_command(Command::format, {.description = "Format C++ source files", .build = build});

}  // namespace

}  // namespace clice::driver
