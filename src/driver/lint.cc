module clice;

import :driver.driver;
import :sched.batch;
import :support.logging;

namespace clice::driver {

namespace {

struct Options {
    deco::decl::HelpOption help;

    DecoFlag(names = {"--index"},
             help = "Also build and persist the project index from the same parses",
             required = false)
    index;

    ProjectOptions where;
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

int run_lint(const ProjectOptions& where, std::uint32_t jobs, bool with_index, bool json) {
    auto read = read_project(where);
    if(!read) {
        return print_error(read.error());
    }
    auto& [root, loaded] = *read;
    auto result = run_batch_lint({
        .project = {.root = std::move(root), .loaded = std::move(loaded), .workers = jobs},
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

CommandHandler build() {
    return operation<Options>("clice lint [OPTIONS]",
                              "lint",
                              Errors::Text,
                              [](const Options& opts) {
                                  return run_lint(opts.where,
                                                  opts.jobs.jobs.value_or(0),
                                                  static_cast<bool>(opts.index),
                                                  static_cast<bool>(opts.report.json));
                              });
}

[[maybe_unused]] const bool registered =
    register_command(Command::lint, {.description = "Lint C++ source files", .build = build});

}  // namespace

}  // namespace clice::driver
