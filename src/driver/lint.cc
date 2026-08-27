#include <print>

#include "driver/driver.h"
#include "sched/batch.h"
#include "support/filesystem.h"
#include "support/logging.h"

#include "llvm/Support/FileSystem.h"

namespace clice::driver {

using kota::deco::decl::KVStyle;

namespace {

struct LintOptions {
    DecoFlag(names = {"-h", "--help"}, help = "Show help", required = false)
    help;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Workspace root directory (default: current directory)",
           required = false)
    <std::string> workspace;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Number of lint workers (default: from config)",
           required = false)
    <std::uint32_t> workers;

    DecoFlag(names = {"--index"},
             help = "Also build and persist the project index from the same parses",
             required = false)
    index;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level;
};

auto make_command() {
    return kota::deco::cli::command<LintOptions>("clice lint [OPTIONS]");
}

int run_lint(std::string root, std::uint32_t workers, bool with_index, const char* self_path) {
    auto result = run_batch_lint(
        {
            .root = std::move(root),
            .workers = workers,
            .self_path = self_path,
            .with_index = with_index,
        },
        [](llvm::StringRef, llvm::ArrayRef<worker::TidyDiagnostic> diagnostics) {
            for(auto& d: diagnostics) {
                std::println("{}:{}:{}: {}: {} [{}]",
                             d.file,
                             d.line,
                             d.column,
                             d.error ? "error" : "warning",
                             d.message,
                             d.check);
            }
        });
    if(result.interrupted) {
        std::println("Lint interrupted. Rerun `clice lint` for a full report.");
        return result.exit_code;
    }
    if(!result.completed) {
        return result.exit_code;
    }
    std::println("Linted {} translation unit{} in {:.1f}s: {} finding{}.",
                 result.checked_tus,
                 plural_s(result.checked_tus),
                 result.seconds,
                 result.findings,
                 plural_s(result.findings));
    if(result.failed_tus != 0) {
        std::println("{} translation unit{} failed to run (see the log); the report is partial.",
                     result.failed_tus,
                     plural_s(result.failed_tus));
    }
    if(result.unsaved) {
        std::println("Part of the index could not be persisted (see the log).");
    }
    return result.exit_code;
}

}  // namespace

void add_lint(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path) {
    auto cmd = make_command();
    cmd.matchAll([&exit_code, self_path](LintOptions opts) {
           if(opts.help) {
               auto help = make_command();
               print_usage(help);
               exit_code = 0;
               return;
           }
           if(!apply_log_level(opts.log_level.value_or("info")))
               return;
           logging::stderr_logger("lint", logging::options);

           llvm::SmallString<256> workspace(opts.workspace.value_or(""));
           if(workspace.empty()) {
               llvm::sys::fs::current_path(workspace);
           } else {
               llvm::sys::fs::make_absolute(workspace);
           }
           std::string ws(workspace.str());
           path::canonicalize(ws);

           exit_code = run_lint(std::move(ws),
                                opts.workers.value_or(0),
                                static_cast<bool>(opts.index),
                                self_path);
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "lint", .description = "Lint C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
