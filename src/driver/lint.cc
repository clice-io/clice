module;

#include "support/prelude.h"

#include "support/logging.macros.h"

module clice;

import :config.config;
import :driver.driver;
import :project.configuration;
import :sched.batch;
import :support.logging;

namespace clice::driver {

namespace {

struct Options {
    kota::deco::decl::HelpOption help;

    DecoFlag(names = {"--index"},
             help = "Also build and persist the project index from the same parses",
             required = false)
    index;

    WorkspaceOptions where;
    JobsOption jobs;
    JsonReportOption report;
    LogLevelOption log;
};

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
             std::uint32_t jobs,
             bool with_index,
             bool json,
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
        .workers = jobs,
        .self_path = self_path,
        .with_index = with_index,
    });
    if(json) {
        print_json(result);
        return result.exit_code;
    }
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
    return operation<Options>("clice lint [OPTIONS]",
                              "lint",
                              Errors::Text,
                              [self_path](const Options& opts) {
                                  auto workspace = opts.where.root();
                                  if(!workspace_exists(workspace)) {
                                      return 2;
                                  }
                                  return run_lint(std::move(workspace),
                                                  opts.where.configuration.tag(),
                                                  opts.jobs.jobs.value_or(0),
                                                  static_cast<bool>(opts.index),
                                                  static_cast<bool>(opts.report.json),
                                                  self_path);
                              });
}

[[maybe_unused]] const bool registered =
    register_command(Command::lint, {.description = "Lint C++ source files", .build = build});

}  // namespace

}  // namespace clice::driver
