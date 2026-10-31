module clice;

import :compile.tidy;
import :driver.driver;
import :project.project;
import :project.unit;
import :support.logging;
import :support.timer;

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

struct Report {
    /// 0 = clean, 1 = findings, 2 = some TUs failed to run or the
    /// requested index could not be persisted (dominates),
    /// 130 = interrupted.
    int exit_code = 0;

    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    std::size_t checked_tus = 0;
    std::size_t failed_tus = 0;

    /// The findings of the run, merged: a finding several TUs produced
    /// identically, notes included (a header's), appears once; sorted by
    /// file, line, column, check.
    std::vector<worker::TidyDiagnostic> findings;

    /// --index only: index state remained that the final save could not
    /// commit; a rerun cannot resume from it.
    bool unsaved = false;

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

/// Sort and merge findings: one per (file, line, column, check, severity,
/// message, notes). The notes stay in the key: a redeclaration check
/// reports the same line from two units with notes pointing at different
/// declarations, and each is a mismatch of its own.
void merge_findings(std::vector<worker::TidyDiagnostic>& findings) {
    ranges::stable_sort(findings, [](auto& a, auto& b) { return finding_key(a) < finding_key(b); });
    auto duplicates =
        ranges::unique(findings, [](auto& a, auto& b) { return finding_key(a) == finding_key(b); });
    findings.erase(duplicates.begin(), duplicates.end());
}

async::task<> lint_one(BatchRun& run, bool with_index, Fid path_id, LintPass& pass) {
    using enum UnitResult::Status;
    auto& project = run.project;
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
    if(!request.tidy && !request.index) {
        if(failed) {
            pass.failed.insert(path_id);
        }
        pass.inflight -= 1;
        pass.task_done.set();
        co_return;
    }

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
        if(failed) {
            pass.failed.insert(path_id);
        }
        pass.inflight -= 1;
        pass.task_done.set();
        co_return;
    }

    switch(result->status) {
        case Completed: {
            if(request.tidy) {
                pass.checked += 1;
            }
            if(with_index) {
                project.apply_report(result->report);
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
            break;
        }
        case Skipped:
        case Failed: {
            // Skipped means no real compile command (the index path keeps
            // the last-known index then; lint has nothing to keep) — either way
            // the TU went unchecked. A command clang rejected says why.
            ranges::move(result->tidy_diagnostics, std::back_inserter(pass.findings));
            failed = true;
            LOG_WARN("Lint failed for {}: {}",
                     file,
                     result->error.empty() ? "no compile command found" : result->error);
            break;
        }
        case Crashed: {
            failed = true;
            LOG_WARN("Lint gave up on {}: {}", file, result->error);
            break;
        }
        case TooBig: {
            failed = true;
            LOG_WARN(
                "Lint gave up on {}: it does not fit in memory, its worker reached {} MiB alone",
                file,
                result->memory >> 20);
            break;
        }
        case Lost:
        case Preempted:
        case OutOfMemory: {
            failed = true;
            LOG_WARN("Lint gave up on {} after a retry: {}", file, result->error);
            break;
        }
    }
    if(failed) {
        pass.failed.insert(path_id);
    }
    pass.inflight -= 1;
    pass.task_done.set();
}

async::task<>
    run_lint_pass(BatchRun& run, bool with_index, llvm::ArrayRef<Fid> tus, LintPass& pass) {
    async::task_group<> workers;

    // The dispatch loop runs as a child of `workers`, like the background
    // index's pass feeder: a cancel cascades through the join and every
    // in-flight task unwinds before the group is destroyed.
    auto feeder = [](BatchRun& run,
                     bool with_index,
                     llvm::ArrayRef<Fid> tus,
                     LintPass& pass,
                     async::task_group<>& workers) -> async::task<> {
        for(auto path_id: tus) {
            // The background index feeder's window: deep enough that
            // workers never idle, shallow enough that a wind-down drains
            // fast.
            while(pass.inflight >= 2 * run.pool.background_limit()) {
                pass.task_done.reset();
                co_await pass.task_done.wait();
            }
            pass.inflight += 1;
            workers.spawn(lint_one(run, with_index, path_id, pass));
        }
    };
    workers.spawn(feeder(run, with_index, tus, pass, workers));
    co_await workers.join();
}

/// Lint the project through unit runs planning {tidy} (or {index, tidy}):
/// open it, run every lintable CDB entry under its .clang-tidy
/// configuration, and return the merged findings. The background index
/// stays off during the pass — the plan's own runs are the only consumer
/// of the graph; with --index it then drains the reindexes the pass's
/// merges owed before the final save.
async::task<> lint_batch(BatchRun& run, BatchProject batch, bool with_index, Report& result) {
    ScopedTimer timer;
    auto& project = run.project;

    if(!start_batch(run, batch, batch.workers, "lint")) {
        result.exit_code = 2;
        // See index_batch: stop the partially started pool or the loop
        // never drains.
        co_await run.pool.stop();
        co_return;
    }
    // The command's product is the lint report: the background pass must
    // not race the plan's own runs, and without --index nothing may touch
    // the persisted index — it opens read-only, so the shutdown save
    // commits nothing.
    project.settings.project.enable_indexing.value = false;

    auto opened = project.open({
        .configuration = batch.loaded.configuration,
        .cache =
            with_index ? Project::OpenOptions::Cache::Write : Project::OpenOptions::Cache::Build,
    });
    project.apply_report(opened.report);
    if(with_index) {
        project.enqueue_outdated(opened.members);
    }

    auto& members = opened.members;
    if(members.empty()) {
        LOG_ERROR("Nothing to lint: {}", project.why_no_members(batch.root));
        result.exit_code = 2;
        co_await shutdown(run);
        co_return;
    }
    if(with_index && !project.database) {
        LOG_ERROR("Cannot persist the index at {}; see the log for the cause and rerun",
                  std::string_view(project.settings.project.cache_dir));
        result.exit_code = 2;
        co_await shutdown(run);
        co_return;
    }

    // One run per file: a file with several CDB entries lints once, under
    // the command resolve_command picks — same as the indexing pass. A TU
    // the rules keep out of the lint set is not even parsed, unless the
    // index wants it.
    llvm::SmallVector<Fid> tus;
    for(auto member: members) {
        auto file = project.file_table.resolve(member);
        if(project.rules.lintable(file) || (with_index && project.rules.indexable(file))) {
            tus.push_back(member);
        }
    }

    Lifetime lifetime(run);
    LintPass pass;
    co_await async::with_token(run_lint_pass(run, with_index, tus, pass), lifetime.token());
    // What arrived is the report, whole or cut short by an interruption.
    for(auto& finding: pass.findings) {
        show_path(project.file_table, finding.file);
        for(auto& note: finding.notes) {
            show_path(project.file_table, note.file);
        }
    }
    merge_findings(pass.findings);
    result.findings = std::move(pass.findings);
    if(with_index && !lifetime.stop_requested) {
        // The pass's merges can owe other TUs a reindex (a rebuilt shared
        // shard dropped their variants), and the opening may have taken
        // the prior session's pending reindexes — the pass runs outside the
        // background index, so nothing drained any of it. Drain it there
        // like the index batch does, or an already-linted owner's index
        // stays missing while the run exits clean.
        project.settings.project.enable_indexing.value = true;
        project.schedule_index(/*immediate=*/true);
        co_await async::with_token(project.wait_index_idle(), lifetime.token());
    }
    if(co_await lifetime.finish()) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.checked_tus = pass.checked;
    // A unit whose pass and reindex both failed fails once.
    for(auto file: project.background_index.failed) {
        pass.failed.insert(file);
    }
    result.failed_tus = pass.failed.size();
    // The shutdown save was the last retry: with --index the persisted
    // index is part of the product, so unsaved state must fail the run
    // like the index-only batch does.
    result.unsaved = with_index && project.has_unsaved_state();
    if(!result.findings.empty()) {
        result.exit_code = 1;
    }
    if(result.failed_tus != 0 || result.unsaved) {
        result.exit_code = 2;
    }
    result.seconds = timer.ms() / 1000.0;
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
