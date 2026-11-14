module clice;

import :compile.tidy;
import :driver.driver;
import :driver.run;
import :project.command;
import :project.graph.unit;
import :project.project;
import :support.logging;
import :support.text;

namespace clice::driver {

namespace {

KANNOTATE_TYPE(struct Lint, description = "Lint C++ source files");
struct Lint {
    KANNOTATE(&index, description = "Also build and persist the project index from the same parses")
    bool index = false;

    ProjectOptions where;
    JobsOption jobs;
    JsonReportOption report;
};

void print_findings(llvm::ArrayRef<worker::TidyDiagnostic> diagnostics) {
    for(auto& d: diagnostics) {
        driver::println("{}{}: {} [{}]{}",
                        location(d.file, d.line, d.column),
                        d.error ? "error" : "warning",
                        d.message,
                        d.check,
                        d.entry ? std::format(" (entry {} of {})", d.entry->index, d.entry->file)
                                : std::string());
        for(auto& note: d.notes) {
            driver::println("{}note: {}",
                            location(note.file, note.line, note.column),
                            note.message);
        }
    }
}

struct Report {
    /// 0 = clean, 1 = findings, 2 = some TUs failed to run or the
    /// requested index could not be persisted (dominates),
    /// 130 = interrupted.
    int exit_code = 0;

    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    /// Why the run stopped before linting: the workers did not start, there
    /// was nothing to lint, or --index found no database to take the index.
    protocol::optional<std::string> error;

    std::size_t checked_tus = 0;
    std::size_t failed_tus = 0;

    /// The findings of the run, merged: a finding several TUs produced
    /// identically, notes included (a header's), appears once, naming an
    /// entry only when no file's first command produced it; sorted by file,
    /// line, column, check.
    std::vector<worker::TidyDiagnostic> findings;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

    /// Why the database refused the last save, when it did.
    protocol::optional<std::string> save_error;

    double seconds = 0;
};

/// The lint pass's shared counters and findings, living on lint_batch's
/// frame.
struct LintPass {
    std::size_t inflight = 0;
    std::size_t checked = 0;
    /// The units that failed, each once however many of its steps did.
    llvm::DenseSet<Fid> failed;
    std::vector<worker::TidyDiagnostic> findings;
    /// The .clang-tidy errors reported so far, each once for all the units
    /// it fails.
    llvm::StringSet<> config_errors;
    async::event task_done{false};
};

auto finding_key(const worker::TidyDiagnostic& d) {
    return std::tie(d.file, d.line, d.column, d.check, d.error, d.message, d.notes);
}

/// The entry a merged finding keeps: none before any, then the smallest.
auto entry_key(const worker::TidyDiagnostic& d) {
    return d.entry ? std::pair(llvm::StringRef(d.entry->file), d.entry->index)
                   : std::pair(llvm::StringRef(), 0U);
}

/// Sort and merge findings: one per (file, line, column, check, severity,
/// message, notes). The notes stay in the key: a redeclaration check
/// reports the same line from two units with notes pointing at different
/// declarations, and each is a mismatch of its own.
void merge_findings(std::vector<worker::TidyDiagnostic>& findings) {
    ranges::stable_sort(findings, [](auto& a, auto& b) {
        return std::pair(finding_key(a), entry_key(a)) < std::pair(finding_key(b), entry_key(b));
    });
    auto duplicates =
        ranges::unique(findings, [](auto& a, auto& b) { return finding_key(a) == finding_key(b); });
    findings.erase(duplicates.begin(), duplicates.end());
}

/// A file of a lint run: compiled under the command resolve_command picks,
/// its first, which alone the index takes, then under each other distinct
/// command of its database entries, one after another: a consumer compiles
/// a file once at a time (Project::compile_unit).
struct LintJob {
    struct Entry {
        /// The first entry with the command (worker::UnitEntry::index).
        std::uint32_t index = 0;

        ContextChoice choice;
    };

    Fid file;

    std::vector<Entry> entries;
};

/// Compile the unit `request` asks for, under the file's entry `entry` when
/// it is not the first command, and add what it found to the pass; whether
/// it failed.
async::task<bool> lint_compile(BatchRun& run,
                               Fid path_id,
                               UnitRequest request,
                               std::optional<std::uint32_t> entry,
                               LintPass& pass) {
    using enum UnitResult::Status;
    auto& project = run.project;
    auto file = project.file_table.resolve(path_id);
    // One retry: a lost compile or a preemption says nothing about the TU,
    // and a one-shot pass has no later pass to requeue into. A compile
    // that crashed its worker would crash the retry too. One whose memory
    // ran out beside other work compiles again until it fits or is found
    // too big: each retry waits for at least the memory the last one
    // reached. Every compile waits while the pool revives a worker: a
    // dispatch then would fail at once for want of one.
    co_await run.pool.await_capacity(false);
    auto result = co_await project.compile_unit(path_id, request);
    // FIXME: unbounded: two units whose allocations fail before the first
    // sample raise their floors by nothing and can die beside each other again
    // and again (commit exhaustion on Windows).
    while(result && result->status == OutOfMemory) {
        co_await run.pool.await_capacity(false);
        result = co_await project.compile_unit(path_id, request);
    }
    if(result && (result->status == Lost || result->status == Preempted)) {
        co_await run.pool.await_capacity(false);
        result = co_await project.compile_unit(path_id, request);
    }
    if(!result) {
        co_return false;
    }
    if(entry) {
        for(auto& d: result->tidy_diagnostics) {
            d.entry = worker::UnitEntry{.file = file.str(), .index = *entry};
        }
    }

    switch(result->status) {
        case Completed: {
            if(request.tidy) {
                pass.checked += 1;
            }
            // The lint set applies to the findings too: a header outside
            // it reports nothing. A compiler error stays wherever it is:
            // broken code is not a clean run. So does a finding no file
            // holds (the configuration's).
            ranges::copy_if(result->tidy_diagnostics,
                            std::back_inserter(pass.findings),
                            [&](const worker::TidyDiagnostic& d) {
                                auto& files = project.file_table;
                                return d.compiler_error || d.file.empty() ||
                                       project.rules.lintable(
                                           files.resolve(files.intern(Spelling::absolute(d.file))));
                            });
            co_return false;
        }
        case Skipped:
        case Failed: {
            // Skipped means no real compile command (the index path keeps
            // the last-known index then; lint has nothing to keep) — either way
            // the TU went unchecked. A command clang rejected says why.
            ranges::move(result->tidy_diagnostics, std::back_inserter(pass.findings));
            LOG_WARN("Lint failed for {}: {}",
                     file,
                     result->error.empty() ? "no compile command found" : result->error);
            co_return true;
        }
        case Crashed: {
            LOG_WARN("Lint gave up on {}: {}", file, result->error);
            co_return true;
        }
        case TooBig: {
            LOG_WARN(
                "Lint gave up on {}: it does not fit in memory, its worker reached {} MiB alone",
                file,
                result->memory >> 20);
            co_return true;
        }
        case Lost:
        case Preempted:
        case OutOfMemory: {
            LOG_WARN("Lint gave up on {} after a retry: {}", file, result->error);
            co_return true;
        }
    }
    std::unreachable();
}

async::task<> lint_one(BatchRun& run, bool with_index, LintJob job, LintPass& pass) {
    auto& project = run.project;
    auto path_id = job.file;
    auto file = project.file_table.resolve(path_id);
    // A TU outside the lint set is here for the index only.
    UnitRequest request;
    request.index = with_index && project.rules.indexable(file);
    bool failed = false;
    if(project.rules.lintable(file)) {
        auto resolution = tidy::resolve_tidy_params(project.file_table.spelling(path_id));
        // Checked under a configuration it does not have, the unit would
        // report what was not asked for and miss what was.
        if(!resolution.error.empty()) {
            if(pass.config_errors.insert(resolution.error).second) {
                LOG_ERROR("{}", resolution.error);
            }
            failed = true;
        } else {
            request.tidy = std::move(resolution.params);
        }
    }
    if(request.tidy || request.index) {
        failed = co_await lint_compile(run, path_id, request, std::nullopt, pass) || failed;
    }
    if(request.tidy) {
        request.index = false;
        for(auto& [index, choice]: job.entries) {
            request.entry = std::move(choice);
            failed = co_await lint_compile(run, path_id, request, index, pass) || failed;
        }
    }
    if(failed) {
        pass.failed.insert(path_id);
    }
    pass.inflight -= 1;
    pass.task_done.set();
}

async::task<>
    run_lint_pass(BatchRun& run, bool with_index, llvm::ArrayRef<LintJob> jobs, LintPass& pass) {
    async::task_group<> workers;

    // The dispatch loop runs as a child of `workers`, like the background
    // index's pass feeder: a cancel cascades through the join and every
    // in-flight task unwinds before the group is destroyed.
    auto feeder = [](BatchRun& run,
                     bool with_index,
                     llvm::ArrayRef<LintJob> jobs,
                     LintPass& pass,
                     async::task_group<>& workers) -> async::task<> {
        for(auto& job: jobs) {
            // The background index feeder's window: deep enough that
            // workers never idle, shallow enough that a wind-down drains
            // fast.
            while(pass.inflight >= 2 * run.pool.background_limit()) {
                pass.task_done.reset();
                co_await pass.task_done.wait();
            }
            pass.inflight += 1;
            workers.spawn(lint_one(run, with_index, job, pass));
        }
    };
    workers.spawn(feeder(run, with_index, jobs, pass, workers));
    co_await workers.join();
}

/// Lint the project through compiles of its units planning {tidy} (or {index, tidy}):
/// open it, run every lintable CDB entry under its .clang-tidy
/// configuration, and return the merged findings. The background index
/// stays off during the pass — the plan's own runs are the only consumer
/// of the graph; with --index it then drains the reindexes the pass's
/// merges owed before the final save.
async::task<> lint_batch(BatchRun& run, BatchProject batch, bool with_index, Report& result) {
    Timer timer;
    auto& project = run.project;

    if(!start_batch(run, batch, batch.workers, "lint")) {
        result.error = "the worker processes did not start";
        result.exit_code = 2;
        // See index_batch: stop the partially started pool or the loop
        // never drains.
        co_await run.pool.stop();
        co_return;
    }
    // The command's product is the lint report: the background index starts
    // only after the plan's own runs, and without --index nothing may touch
    // the persisted index — it opens read-only, so the shutdown save
    // commits nothing.
    auto opened = project.open({
        .configuration = batch.loaded.configuration,
        .cache =
            with_index ? Project::OpenOptions::Cache::Write : Project::OpenOptions::Cache::Build,
    });

    auto& members = opened.members;
    if(members.empty()) {
        result.error = std::format("Nothing to lint: {}", project.why_no_members(batch.root));
    } else if(with_index && !project.database) {
        result.error = std::format("Cannot persist the index at {}: {}",
                                   std::string_view(project.settings.project.cache_dir),
                                   opened.unwritable);
    }
    if(result.error) {
        result.exit_code = 2;
        co_await shutdown(run);
        co_return;
    }

    // One run per command a file compiles under: a file several database
    // entries list lints under each distinct command, as clang-tidy runs
    // each, the index taking the first. A TU the rules keep out of the lint
    // set is not even parsed, unless the index wants it.
    std::vector<LintJob> jobs;
    for(auto member: members) {
        auto file = project.file_table.resolve(member);
        bool lintable = project.rules.lintable(file);
        if(!lintable && !(with_index && project.rules.indexable(file))) {
            continue;
        }
        auto& job = jobs.emplace_back(LintJob{.file = member});
        if(!lintable) {
            continue;
        }
        llvm::StringSet<> commands;
        for(auto [i, candidate]: views::enumerate(project.candidates(member))) {
            auto base = project.commands.command_hash_hex(candidate.base);
            if(commands.insert(base).second && commands.size() > 1) {
                job.entries.push_back({.index = static_cast<std::uint32_t>(i + 1),
                                       .choice = ContextChoice{.base = std::move(base)}});
            }
        }
    }

    Lifetime lifetime(run);
    LintPass pass;
    co_await async::with_token(run_lint_pass(run, with_index, jobs, pass), lifetime.stop.token());
    // What arrived is the report, whole or cut short by an interruption.
    for(auto& finding: pass.findings) {
        show_path(project.file_table, finding.file);
        for(auto& note: finding.notes) {
            show_path(project.file_table, note.file);
        }
        if(finding.entry) {
            show_path(project.file_table, finding.entry->file);
        }
    }
    merge_findings(pass.findings);
    result.findings = std::move(pass.findings);
    auto& background = project.background_index;
    if(with_index && !lifetime.stop_requested) {
        // The pass's merges can owe other TUs a reindex (a rebuilt shared
        // shard dropped their variants), and the opening may have taken
        // the prior session's pending reindexes — the pass runs outside the
        // background index, so nothing drained any of it. Drain it there
        // like the index batch does, or an already-linted owner's index
        // stays missing while the run exits clean.
        background.start();
        background.schedule(/*immediate=*/true);
        co_await async::with_token(background.wait_idle(), lifetime.stop.token());
    }
    if(co_await lifetime.finish()) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.checked_tus = pass.checked;
    // A unit whose pass and reindex both failed fails once.
    for(auto& [file, attempt]: background.last_attempts()) {
        if(attempt.failed) {
            pass.failed.insert(file);
        }
    }
    result.failed_tus = pass.failed.size();
    // The shutdown save was the last retry: with --index the persisted
    // index is part of the product, so unsaved state must fail the run
    // like the index-only batch does.
    result.unsaved = with_index && project.has_unsaved_state();
    if(!project.save_error.empty()) {
        result.save_error = project.save_error;
    }
    if(!result.findings.empty()) {
        result.exit_code = 1;
    }
    if(result.failed_tus != 0 || result.unsaved) {
        result.exit_code = 2;
    }
    result.seconds = timer.ms() / 1000.0;
    LOG_INFO("Linted {} in {} ms: {}, {} failed",
             counted(result.checked_tus, "translation unit", "translation units"),
             timer.ms(),
             counted(result.findings.size(), "finding", "findings"),
             result.failed_tus);
}

int run_lint(const ProjectOptions& where, std::uint32_t jobs, bool with_index, bool json) {
    auto read = read_project(where);
    if(!read) {
        return print_error(read.error());
    }
    auto& [root, loaded] = *read;
    async::event_loop loop;
    BatchRun run(loop);
    Report result;
    loop.schedule(
        lint_batch(run,
                   {.root = std::move(root), .loaded = std::move(loaded), .workers = jobs},
                   with_index,
                   result));
    loop.run();
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
        if(result.error) {
            LOG_ERROR("{}", *result.error);
        }
        return result.exit_code;
    }
    driver::println("Linted {} in {:.1f}s: {}.",
                    counted(result.checked_tus, "translation unit", "translation units"),
                    result.seconds,
                    counted(result.findings.size(), "finding", "findings"));
    if(result.failed_tus != 0) {
        driver::println("{} failed to run (see the log); the report is partial.",
                        counted(result.failed_tus, "translation unit", "translation units"));
    }
    if(result.unsaved) {
        driver::println("Part of the index could not be persisted{}.",
                        result.save_error ? std::format(": {}", *result.save_error)
                                          : std::string());
    }
    return result.exit_code;
}

int run(Lint& lint) {
    return run_lint(lint.where, lint.jobs.jobs, lint.index, lint.report.json);
}

[[maybe_unused]] const bool added = cli::add_command<Subcommands>(dispatch<Lint>);

}  // namespace

}  // namespace clice::driver
