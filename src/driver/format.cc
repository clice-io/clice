#include <print>

#include "driver/driver.h"
#include "sched/batch.h"
#include "support/filesystem.h"
#include "support/logging.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct FormatOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

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

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level;

    DecoInput(meta_var = "<PATH>...",
              help =
                  "Files to format, or directories to format the build's own files under; "
                  "relative to the workspace (default: the build's own files)",
              required = false)
    <std::vector<std::string>> paths;
};

auto make_command() {
    return kota::deco::cli::command<FormatOptions>("clice format [OPTIONS] [<PATH>...]");
}

/// An argument as the build spells paths: absolute under the workspace,
/// dot segments folded, canonical.
std::string argument_path(llvm::StringRef root, llvm::StringRef argument) {
    llvm::SmallString<256> absolute(path::is_absolute(argument) ? argument.str()
                                                                : path::join(root, argument));
    path::remove_dots(absolute, /*remove_dot_dot=*/true);
    std::string result(absolute.str());
    path::canonicalize(result);
    return result;
}

int run_format(BatchFormatOptions options) {
    auto result = run_batch_format(options);
    std::print(stderr, "{}", result.output);
    if(result.exit_code == 2) {
        std::println(stderr, "{}", result.error);
        return 2;
    }
    if(result.files == 0) {
        std::println("No files to format under {}.", options.root);
        return 0;
    }
    if(!options.check) {
        std::println("Formatted {} file{} in {:.1f}s.",
                     result.files,
                     plural_s(result.files),
                     result.seconds);
    } else if(result.unformatted.empty()) {
        std::println("Checked {} file{} in {:.1f}s: all formatted.",
                     result.files,
                     plural_s(result.files),
                     result.seconds);
    } else {
        std::println("Checked {} file{} in {:.1f}s: {} need{} formatting.",
                     result.files,
                     plural_s(result.files),
                     result.seconds,
                     result.unformatted.size(),
                     result.unformatted.size() == 1 ? "s" : "");
    }
    return result.exit_code;
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
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           logging::stderr_logger("format", logging::options);

           BatchFormatOptions options{
               .root = workspace_root(opts.workspace.value_or("")),
               .configuration = opts.configuration.value_or(""),
               .clang_format = opts.clang_format.value_or("clang-format"),
               .jobs = opts.jobs.value_or(0),
               .check = static_cast<bool>(opts.check),
           };
           for(auto& argument: opts.paths.value_or(std::vector<std::string>{})) {
               options.paths.push_back(argument_path(options.root, argument));
           }
           exit_code = run_format(std::move(options));
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "format", .description = "Format C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
