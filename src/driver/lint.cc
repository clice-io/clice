module;

#include "modules/prelude.h"

#include "support/logging.macros.h"

module clice;

import :config.config;
import :driver.driver;
import :project.configuration;
import :sched.batch;
import :support.logging;

namespace clice::driver {

namespace {

struct LintOptions {
    kota::deco::decl::HelpOption help;

    DecoKV(help = "Workspace root directory (default: current directory)", required = false)
    <std::string> workspace;

    DecoKV(help =
               "Build configuration to activate, one of the tags declared on rules "
               "(default: the selected one, else default_configuration)",
           required = false)
    <std::string> configuration;

    DecoKV(help = "Number of lint workers (default: from config)", required = false)
    <std::uint32_t> workers;

    DecoFlag(names = {"--index"},
             help = "Also build and persist the project index from the same parses",
             required = false)
    index;

    LogLevelOption log;
};

/// Where a finding or a note is, as compilers print it; nothing for one no
/// file holds.
std::string location(llvm::StringRef file, std::uint32_t line, std::uint32_t column) {
    if(file.empty()) {
        return {};
    }
    return std::format("{}:{}:{}: ", file, line, column);
}

void print_findings(llvm::ArrayRef<worker::TidyDiagnostic> diagnostics) {
    for(auto& d: diagnostics) {
        driver::println("{}{}: {} [{}]",
                        location(d.file, d.line, d.column),
                        d.error ? "error" : "warning",
                        d.message,
                        d.check);
        for(auto& note: d.notes) {
            driver::println("{}note: {}",
                            location(note.file, note.line, note.column),
                            note.message);
        }
    }
}

int run_lint(Spelling root,
             std::string configuration,
             std::uint32_t workers,
             bool with_index,
             const char* self_path) {
    auto config = Config::load_from_workspace(CanonicalPath(root));
    if(auto checked = check_requested_configuration(config, configuration); !checked) {
        LOG_ERROR("{}", checked.error());
        return 2;
    }
    auto result = run_batch_lint({
        .root = std::move(root),
        .config = std::move(config),
        .configuration = std::move(configuration),
        .workers = workers,
        .self_path = self_path,
        .with_index = with_index,
    });
    print_findings(result.findings);
    if(result.interrupted) {
        driver::println("Lint interrupted. Rerun `clice lint` for a full report.");
        return result.exit_code;
    }
    if(!result.completed) {
        return result.exit_code;
    }
    driver::println("Linted {} translation unit{} in {:.1f}s: {} finding{}.",
                    result.checked_tus,
                    plural_s(result.checked_tus),
                    result.seconds,
                    result.findings.size(),
                    plural_s(result.findings.size()));
    if(result.failed_tus != 0) {
        driver::println("{} translation unit{} failed to run (see the log); the report is partial.",
                        result.failed_tus,
                        plural_s(result.failed_tus));
    }
    if(result.unsaved) {
        driver::println("Part of the index could not be persisted (see the log).");
    }
    return result.exit_code;
}

CommandHandler build(const char* self_path) {
    auto cmd = kota::deco::cli::command<LintOptions>("clice lint [OPTIONS]");
    cmd.match_all([self_path](LintOptions opts) {
        opts.log.apply();
        logging::stderr_logger("lint", logging::options);

        auto workspace = workspace_spelling(opts.workspace.value_or(""));
        if(!workspace_exists(workspace)) {
            return 2;
        }
        return run_lint(std::move(workspace),
                        opts.configuration.value_or(""),
                        opts.workers.value_or(0),
                        static_cast<bool>(opts.index),
                        self_path);
    });
    return cmd;
}

[[maybe_unused]] const bool registered =
    register_command(Command::lint, {.description = "Lint C++ source files", .build = build});

}  // namespace

}  // namespace clice::driver
