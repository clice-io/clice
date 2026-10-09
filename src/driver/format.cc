module;

#include "modules/prelude.h"

module clice;

import :driver.driver;
import :sched.batch;
import :support.logging;
import :vfs.path;

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct FormatOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help =
               "Build configuration to activate, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoFlag(names = {"--check"},
             help = "Report the files that need formatting instead of rewriting them",
             required = false)
    check;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--clang-format", "--clang-format="},
           help = "The clang-format executable to run (default: clang-format from PATH)",
           required = false)
    <std::string> clang_format;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Number of clang-format processes to run at once (default: the CPU count)",
           required = false)
    <std::uint32_t> jobs;

    LogLevelOption log;

    DecoInput(meta_var = "<PATH>...",
              help =
                  "Files to format, or directories to format the build's own files under; "
                  "relative to the workspace (default: the build's own files)",
              required = false)
    <std::vector<std::string>> paths;
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

}  // namespace

void add_format(kota::deco::cli::SubCommander& root) {
    auto cmd = kota::deco::cli::command<FormatOptions>("clice format [OPTIONS] [<PATH>...]");
    cmd.match_all([](FormatOptions opts) {
        opts.log.apply();
        logging::stderr_logger("format", logging::options);

        BatchFormatOptions options{
            .root = workspace_root(opts.workspace.value_or("")),
            .configuration = opts.configuration.value_or(""),
            .clang_format = opts.clang_format.value_or("clang-format"),
            .jobs = opts.jobs.value_or(0),
            .check = static_cast<bool>(opts.check),
        };
        for(auto& argument: opts.paths.value_or(std::vector<std::string>{})) {
            options.paths.emplace_back(argument, Spelling(options.root));
        }
        return run_format(std::move(options));
    });

    root.add({.name = "format", .description = "Format C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
