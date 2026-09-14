#include <print>

#include "driver/driver.h"
#include "sched/batch.h"
#include "support/logging.h"

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
           help =
               "Build configuration to activate, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           help = "Number of lint workers (default: from config)",
           required = false)
    <std::uint32_t> workers;

    DecoFlag(names = {"--index"},
             help = "Also build and persist the project index from the same parses",
             required = false)
    index;

    DecoFlag(names = {"--no-dedup"},
             help =
                 "Check every translation unit whole instead of checking each shared "
                 "declaration once across the run",
             required = false)
    no_dedup;

    DecoFlag(names = {"--verify"},
             help =
                 "Also check every translation unit whole and fail when the "
                 "deduplicated findings differ from that baseline",
             required = false)
    verify;

    DecoKV(style = KVStyle::JoinedOrSeparate,
           names = {"--log-level", "--log-level="},
           help = "Log level: trace, debug, info, warn, error, off",
           required = false)
    <std::string> log_level;
};

auto make_command() {
    return kota::deco::cli::command<LintOptions>("clice lint [OPTIONS]");
}

void print_findings(llvm::ArrayRef<worker::TidyDiagnostic> diagnostics) {
    for(auto& d: diagnostics) {
        std::println("{}:{}:{}: {}: {} [{}]",
                     d.file,
                     d.line,
                     d.column,
                     d.error ? "error" : "warning",
                     d.message,
                     d.check);
        for(auto& note: d.notes) {
            std::println("{}:{}:{}: note: {}", note.file, note.line, note.column, note.message);
        }
    }
}

int run_lint(const BatchLintOptions& options) {
    auto result = run_batch_lint(options);
    print_findings(result.findings);
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
                 result.findings.size(),
                 plural_s(result.findings.size()));
    if(options.verify) {
        if(result.verify_missing.empty() && result.verify_extra.empty()) {
            std::println("Verification passed: the deduplicated findings match the whole runs.");
        } else {
            std::println(
                "Verification failed: {} finding{} lost, {} invented by deduplication "
                "(see the log).",
                result.verify_missing.size(),
                plural_s(result.verify_missing.size()),
                result.verify_extra.size());
        }
    }
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

           exit_code = run_lint({
               .root = workspace_root(opts.workspace.value_or("")),
               .configuration = opts.configuration.value_or(""),
               .workers = opts.workers.value_or(0),
               .self_path = self_path,
               .with_index = static_cast<bool>(opts.index),
               .dedup = !static_cast<bool>(opts.no_dedup),
               .verify = static_cast<bool>(opts.verify),
           });
       })
        .on_error([](auto err) { LOG_ERROR("{}", err.message); });

    root.add({.name = "lint", .description = "Lint C++ source files"}, std::move(cmd));
}

}  // namespace clice::driver
