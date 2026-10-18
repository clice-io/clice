module;

#include "modules/prelude.h"

#include "support/anomaly.macros.h"
#include "support/logging.macros.h"

module clice;

import :command.command;
import :compile.tidy;
import :config.config;
import :feature.feature;
import :project.build;
import :project.command_resolver;
import :project.configuration;
import :project.index_store;
import :project.load;
import :project.project;
import :sched.batch;
import :sched.bootstrap;
import :sched.index.pump;
import :sched.stack;
import :support.anomaly;
import :support.logging;
import :support.process;
import :support.signal;
import :support.timer;
import :vfs.cache_store;
import :vfs.file_system;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;

namespace clice {

namespace {

/// A path a worker reported, as the user knows the file; empty for none.
void show_path(FileTable& files, std::string& path) {
    if(!path.empty()) {
        path = files.display(files.intern(Spelling::absolute(path)));
    }
}

/// The lean batch assembly: the scheduling stack the server also runs on,
/// minus everything serving-side.
struct BatchStack {
    kota::event_loop& loop;
    FileTable files;
    Project project{files};
    CommandResolver commands{project};
    WorkerPool pool{loop};
    SchedulingStack sched{loop, project, commands, pool};

    /// The session log directory start_batch created; empty when file
    /// logging is off.
    std::string log_dir;

    explicit BatchStack(kota::event_loop& loop) : loop(loop) {}
};

kota::task<> checkpoint_task(BatchStack& stack) {
    while(true) {
        // Time is the feature: the checkpoints' pace.
        co_await kota::sleep(SchedulingStack::checkpoint_interval());
        co_await stack.sched.checkpoint();
    }
}

/// The current round's progress to the driver's callback; nothing before
/// the first round has a total.
void report_progress(BatchStack& stack, const BatchOptions& options) {
    auto& round = stack.sched.pump.progress();
    if(!options.on_progress || round.total == 0) {
        return;
    }
    options.on_progress({.completed = round.completed,
                         .total = round.total,
                         .failed = stack.sched.pump.failed().size()});
}

/// A paced report on top of the round boundaries: one unit can take
/// longer than the pace, and a run must not fall silent while it runs.
kota::task<> progress_ticker(BatchStack& stack, const BatchOptions& options) {
    constexpr auto pace = std::chrono::seconds(10);
    while(true) {
        // Time is the feature: the report's pace.
        co_await kota::sleep(pace);
        report_progress(stack, options);
    }
}

/// Test hook: CLICE_TEST_PAUSE="<units> <file>" parks a run once that many
/// units are indexed, nothing else dispatched, and creates the file, so a
/// test can signal the run at a known point.
struct TestPause {
    std::size_t units = 0;
    std::string file;
    bool reached = false;
};

std::optional<TestPause> test_pause() {
    auto value = llvm::sys::Process::GetEnv("CLICE_TEST_PAUSE");
    if(!value) {
        return std::nullopt;
    }
    auto [count, file] = llvm::StringRef(*value).split(' ');
    TestPause pause;
    if(count.getAsInteger(10, pause.units) || pause.units == 0 || file.empty()) {
        LOG_WARN("CLICE_TEST_PAUSE is '<units> <file>', not '{}'", *value);
        return std::nullopt;
    }
    pause.file = file.str();
    return pause;
}

/// Quiesce the pump, then the shared contract-11 tail.
kota::task<> shutdown(BatchStack& stack) {
    co_await stack.sched.pump.stop();
    co_await stack.sched.shutdown();
    co_await stack.pool.stop();
    stack.sched.close();
}

/// A batch run's signal handling and the order of its ending. Interruption
/// is judged only after the scheduling stack shut down and the watchers
/// finished: a signal arriving while the final save/teardown ran must still
/// report an interruption, not a normal completion with exit code 0.
struct BatchLifetime {
    BatchStack& stack;
    bool stop_requested = false;
    kota::cancellation_source stop;
    kota::task_group<> aux;

    explicit BatchLifetime(BatchStack& stack) : stack(stack) {
        // Every signal stops gracefully: in-flight files are abandoned,
        // finished ones are persisted, and a rerun resumes from there. A
        // closed terminal sends SIGHUP.
        auto interrupt = [this] {
            LOG_INFO("Interrupted; saving indexing progress");
            stop.cancel();
        };
        aux.spawn(watch_termination(SIGINT, stop_requested, interrupt));
        aux.spawn(watch_termination(SIGTERM, stop_requested, interrupt));
#ifndef _WIN32
        aux.spawn(watch_termination(SIGHUP, stop_requested, interrupt));
#endif
        aux.spawn(checkpoint_task(stack));
        aux.spawn(stack.files.disk.end_turns(stack.loop));
    }

    kota::cancellation_token token() {
        return stop.token();
    }

    /// Shuts the stack down and stops the watchers; whether the run was
    /// interrupted.
    kota::task<bool> finish() {
        co_await shutdown(stack);
        aux.cancel();
        co_await aux.join();
        co_return stop_requested;
    }
};

/// Shared batch startup: the command's config with the run's overrides,
/// the session file logger, and the worker pool. `log_tag` names the log
/// files after the subcommand.
bool start_batch(BatchStack& stack,
                 Config config,
                 const Spelling& root,
                 std::uint32_t workers,
                 llvm::StringRef self_path,
                 llvm::StringRef log_tag) {
    auto& project = stack.project;
    stack.files.spell_root(root);
    project.config = std::move(config);
    auto& cfg = project.config.project;
    cfg.idle_timeout_ms.value = 0;
    if(workers != 0) {
        cfg.stateless_worker_count.value = workers;
        cfg.min_stateless_worker_count.value = workers;
        cfg.max_stateless_worker_count.value = workers;
    }

    if(cfg.cache_dir_defaulted.value) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }

    std::string session_log_dir;
    if(!cfg.logging_dir.empty()) {
        session_log_dir = logging::session_log_directory(cfg.logging_dir);
        if(logging::file_logger(log_tag, session_log_dir, logging::options)) {
            LOG_INFO("Session log directory: {}", session_log_dir);
            stack.log_dir = session_log_dir;
        }
    }

    WorkerPoolOptions pool_opts;
    pool_opts.self_path = std::string(self_path);
    // Stateful workers host open documents; a batch run has none.
    pool_opts.stateful_count = 0;
    pool_opts.stateless_count = cfg.stateless_worker_count;
    pool_opts.min_stateless = cfg.min_stateless_worker_count;
    pool_opts.max_stateless = cfg.max_stateless_worker_count;
    pool_opts.log_dir = session_log_dir;
    if(!stack.pool.start(pool_opts)) {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
        return false;
    }
    return true;
}

kota::task<> run(BatchStack& stack, const BatchOptions& options, BatchResult& result) {
    ScopedTimer timer;
    auto& project = stack.project;

    bool started = start_batch(stack,
                               options.config,
                               options.root,
                               options.workers,
                               options.self_path,
                               "index");
    result.log_dir = stack.log_dir;
    if(!started) {
        result.exit_code = 1;
        // A failed later spawn leaves earlier workers and their I/O tasks
        // live; unstopped they keep the batch event loop spinning and the
        // command hangs instead of exiting.
        co_await stack.pool.stop();
        co_return;
    }
    project.config.project.enable_indexing.value = true;
    stack.sched.pump.collect_errors();

    auto report = bootstrap_project(project,
                                    stack.sched.store,
                                    stack.sched.pump,
                                    project.config.workspace_root,
                                    options.configuration);

    // The command's whole product is the persisted index: without storage
    // (cache failed to open, another process holds the index writer lock,
    // or an unreadable global blob disabled persistence) the run would
    // only warm this process's memory and a rerun would start from
    // nothing — fail instead of pretending.
    if(!project.index_db) {
        LOG_ERROR("Cannot persist the index at {}: the warning above says why; fix that and rerun",
                  std::string_view(project.config.project.cache_dir));
        result.exit_code = 1;
        co_await shutdown(stack);
        co_return;
    }
    if(report.members.empty()) {
        LOG_ERROR(
            "Nothing to index: no compile_commands.json found in {} or its direct "
            "subdirectories.{}",
            options.root,
            deeper_databases_hint(project));
        result.exit_code = 1;
        co_await shutdown(stack);
        co_return;
    }

    auto progress = stack.sched.pump.on_progress_changed.connect([&] {
        if(stack.sched.pump.progress().stage != IndexPump::Progress::Stage::Report) {
            report_progress(stack, options);
        }
    });
    Signal<>::Connection parked;
    if(auto pause = test_pause()) {
        stack.sched.pump.pause_after(pause->units);
        parked = stack.sched.pump.on_progress_changed.connect([&stack, pause = *pause]() mutable {
            if(!pause.reached && stack.sched.pump.progress().completed >= pause.units) {
                pause.reached = true;
                if(auto error = vfs::write(pause.file, "")) {
                    LOG_WARN("CLICE_TEST_PAUSE cannot create {}: {}", pause.file, error.message());
                }
            }
        });
    }
    BatchLifetime lifetime(stack);
    lifetime.aux.spawn(progress_ticker(stack, options));
    co_await kota::with_token(stack.sched.pump.wait_idle(), lifetime.token());
    auto interrupted = co_await lifetime.finish();
    // The shutdown save was the last retry for failed writes; whatever is
    // still dirty never reached disk and a rerun cannot resume from it.
    result.unsaved = stack.sched.store.has_unsaved_state();
    if(interrupted) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.indexed_tus = stack.sched.pump.indexed_files();
    for(auto tu: llvm::make_first_range(project.project_index.manifests)) {
        if(!project.build.unit(tu)) {
            result.standalone_headers += 1;
        }
    }
    result.shard_count = project.project_index.shards.size();
    for(auto& shard: llvm::make_second_range(project.project_index.shards)) {
        result.shard_bytes += shard.bytes().size();
    }
    result.symbol_count = project.project_index.symbol_count();
    for(auto file: stack.sched.pump.failed()) {
        result.failed.emplace_back(project.file_table.display(file));
    }
    std::ranges::sort(result.failed);
    for(auto& [unit, errors]: stack.sched.pump.compiled_with_errors()) {
        auto& shown = result.compiled_with_errors.emplace_back(
            BatchResult::UnitErrors{.unit = project.file_table.display(unit), .errors = errors});
        for(auto& error: shown.errors) {
            show_path(project.file_table, error.file);
        }
    }
    std::ranges::sort(result.compiled_with_errors, {}, &BatchResult::UnitErrors::unit);
    if(!result.failed.empty() || result.unsaved) {
        result.exit_code = 1;
    }
    result.seconds = timer.ms() / 1000.0;
}

/// The lint pass's shared counters and findings, living on run_lint's
/// frame.
struct LintPass {
    std::size_t inflight = 0;
    std::size_t checked = 0;
    std::size_t failed = 0;
    std::vector<worker::TidyDiagnostic> findings;
    /// The .clang-tidy errors reported so far, each once for all the units
    /// it fails.
    llvm::StringSet<> config_errors;
    kota::event task_done{false};
};

auto finding_key(const worker::TidyDiagnostic& d) {
    return std::tie(d.file, d.line, d.column, d.check, d.error, d.message, d.notes);
}

/// Sort and merge findings: one per (file, line, column, check, severity,
/// message, notes). The notes stay in the key: a redeclaration check
/// reports the same line from two units with notes pointing at different
/// declarations, and each is a mismatch of its own.
void merge_findings(std::vector<worker::TidyDiagnostic>& findings) {
    std::ranges::stable_sort(findings,
                             [](auto& a, auto& b) { return finding_key(a) < finding_key(b); });
    auto duplicates = std::ranges::unique(findings, [](auto& a, auto& b) {
        return finding_key(a) == finding_key(b);
    });
    findings.erase(duplicates.begin(), duplicates.end());
}

kota::task<> lint_one(BatchStack& stack, bool with_index, Fid path_id, LintPass& pass) {
    auto file = stack.project.file_table.resolve(path_id);
    // A TU outside the lint set is here for the index only.
    TURunFamily::Plan plan;
    plan.index = with_index && stack.project.build.indexed(file);
    // A unit fails once, however many of its steps do.
    bool failed = false;
    if(stack.project.build.lintable(file)) {
        auto resolution = tidy::resolve_tidy_params(stack.project.file_table.spelling(path_id));
        // Checked under a configuration it does not have, the unit would
        // report what was not asked for and miss what was.
        if(!resolution.error.empty()) {
            if(pass.config_errors.insert(resolution.error).second) {
                LOG_ERROR("{}", resolution.error);
            }
            failed = true;
        } else {
            plan.tidy = std::move(resolution.params);
        }
    }
    if(!plan.tidy && !plan.index) {
        if(failed) {
            pass.failed += 1;
        }
        pass.inflight -= 1;
        pass.task_done.set();
        co_return;
    }

    // One retry: a lost run or a preemption says nothing about the TU, and
    // a one-shot pass has no later round to requeue into. A run that
    // crashed its worker would crash the retry too.
    auto outcome = co_await stack.sched.turun.run(path_id, plan);
    if(outcome.result == TURunFamily::Result::Lost ||
       outcome.result == TURunFamily::Result::Preempted) {
        outcome = co_await stack.sched.turun.run(path_id, std::move(plan));
    }

    switch(outcome.result) {
        case TURunFamily::Result::Completed: {
            if(plan.tidy) {
                pass.checked += 1;
            }
            if(with_index) {
                stack.sched.pump.apply_report(outcome.report);
            }
            // The lint set applies to the findings too: a header outside
            // it reports nothing. A compiler error stays wherever it is:
            // broken code is not a clean run. So does a finding no file
            // holds (the configuration's).
            std::ranges::copy_if(outcome.tidy_diagnostics,
                                 std::back_inserter(pass.findings),
                                 [&](const worker::TidyDiagnostic& d) {
                                     auto& files = stack.project.file_table;
                                     return d.compiler_error || d.file.empty() ||
                                            stack.project.build.lintable(files.resolve(
                                                files.intern(Spelling::absolute(d.file))));
                                 });
            break;
        }
        case TURunFamily::Result::Skipped:
        case TURunFamily::Result::Failed: {
            // Skipped means no real compile command (the index path keeps
            // the last-known index then; lint has nothing to keep) — either way
            // the TU went unchecked. A command clang rejected says why.
            std::ranges::move(outcome.tidy_diagnostics, std::back_inserter(pass.findings));
            failed = true;
            LOG_WARN("Lint failed for {}: {}",
                     file,
                     outcome.error.empty() ? "no compile command found" : outcome.error);
            break;
        }
        case TURunFamily::Result::Crashed: {
            failed = true;
            LOG_WARN("Lint gave up on {}: {}", file, outcome.error);
            break;
        }
        case TURunFamily::Result::Lost:
        case TURunFamily::Result::Preempted: {
            failed = true;
            LOG_WARN("Lint gave up on {} after a retry: {}", file, outcome.error);
            break;
        }
        case TURunFamily::Result::Shutdown: {
            break;
        }
    }
    if(failed) {
        pass.failed += 1;
    }
    pass.inflight -= 1;
    pass.task_done.set();
}

kota::task<> run_lint_pass(BatchStack& stack,
                           const BatchLintOptions& options,
                           llvm::ArrayRef<Fid> tus,
                           LintPass& pass) {
    kota::task_group<> workers;

    // The dispatch loop runs as a child of `workers`, like the pump's
    // round feeder: a cancel cascades through the join and every in-flight
    // task unwinds before the group is destroyed.
    auto feeder = [](BatchStack& stack,
                     const BatchLintOptions& options,
                     llvm::ArrayRef<Fid> tus,
                     LintPass& pass,
                     kota::task_group<>& workers) -> kota::task<> {
        for(auto path_id: tus) {
            // The pump feeder's window: deep enough that workers never
            // idle, shallow enough that a wind-down drains fast.
            while(pass.inflight >= std::max<std::size_t>(2 * stack.pool.effective_low_limit(), 2)) {
                pass.task_done.reset();
                co_await pass.task_done.wait();
            }
            pass.inflight += 1;
            workers.spawn(lint_one(stack, options.with_index, path_id, pass));
        }
    };
    workers.spawn(feeder(stack, options, tus, pass, workers));
    co_await workers.join();
}

kota::task<> run_lint(BatchStack& stack, const BatchLintOptions& options, BatchLintResult& result) {
    ScopedTimer timer;
    auto& project = stack.project;

    if(!start_batch(stack,
                    options.config,
                    options.root,
                    options.workers,
                    options.self_path,
                    "lint")) {
        result.exit_code = 2;
        // See run(): stop the partially started pool or the loop never
        // drains.
        co_await stack.pool.stop();
        co_return;
    }
    // The command's product is the lint report: the background pass must
    // not race the plan's own runs, and without --index nothing may touch
    // the persisted index — the read-only load queues no reconciliation
    // or removal writes, so the shutdown save commits nothing.
    project.config.project.enable_indexing.value = false;

    auto report = bootstrap_project(project,
                                    stack.sched.store,
                                    stack.sched.pump,
                                    project.config.workspace_root,
                                    options.configuration,
                                    /*read_only_index=*/!options.with_index);

    auto& members = report.members;
    if(members.empty()) {
        LOG_ERROR(
            "Nothing to lint: no compile_commands.json found in {} or its direct "
            "subdirectories.{}",
            options.root,
            deeper_databases_hint(project));
        result.exit_code = 2;
        co_await shutdown(stack);
        co_return;
    }
    if(options.with_index && !project.index_db) {
        LOG_ERROR("Cannot persist the index at {}; see the log for the cause and rerun",
                  std::string_view(project.config.project.cache_dir));
        result.exit_code = 2;
        co_await shutdown(stack);
        co_return;
    }

    // One run per file: a file with several CDB entries lints once, under
    // the command resolve_command picks — same as the indexing pass. A TU
    // the rules keep out of the lint set is not even parsed, unless the
    // index wants it.
    llvm::SmallVector<Fid> tus;
    for(auto member: members) {
        auto file = project.file_table.resolve(member);
        if(project.build.lintable(file) || (options.with_index && project.build.indexed(file))) {
            tus.push_back(member);
        }
    }

    BatchLifetime lifetime(stack);
    LintPass pass;
    co_await kota::with_token(run_lint_pass(stack, options, tus, pass), lifetime.token());
    // What arrived is the report, whole or cut short by an interruption.
    for(auto& finding: pass.findings) {
        show_path(project.file_table, finding.file);
        for(auto& note: finding.notes) {
            show_path(project.file_table, note.file);
        }
    }
    merge_findings(pass.findings);
    result.findings = std::move(pass.findings);
    if(options.with_index && !lifetime.stop_requested) {
        // The pass's merges can owe other TUs a reindex (a rebuilt shared
        // shard dropped their variants), and bootstrap may have taken
        // prior-session debt — the pass runs outside the pump, so nothing
        // drained any of it. Drain it through the pump like the index
        // batch does, or an already-linted owner's index stays missing while
        // the run exits clean.
        project.config.project.enable_indexing.value = true;
        stack.sched.pump.schedule(/*immediate=*/true);
        co_await kota::with_token(stack.sched.pump.wait_idle(), lifetime.token());
    }
    if(co_await lifetime.finish()) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.checked_tus = pass.checked;
    result.failed_tus = pass.failed + stack.sched.pump.failed().size();
    // The shutdown save was the last retry: with --index the persisted
    // index is part of the product, so unsaved state must fail the run
    // like the index-only batch does.
    result.unsaved = options.with_index && stack.sched.store.has_unsaved_state();
    if(!result.findings.empty()) {
        result.exit_code = 1;
    }
    if(result.failed_tus != 0 || result.unsaved) {
        result.exit_code = 2;
    }
    result.seconds = timer.ms() / 1000.0;
}

/// What clang-format can format: the C family clang derives types from,
/// and `.cuh`, which clang does not list but CUDA code uses.
bool formats(llvm::StringRef path) {
    return clang::driver::types::isDerivedFromC(suffix_type(path)) ||
           path::extension(path) == ".cuh";
}

/// A file to format: read and rewritten where its bytes are, which keeps a
/// symlink a link, and formatted under the name the build or the command
/// line gives it, which picks its style as an open document's name does.
struct FormatFile {
    CanonicalPath path;
    Spelling name;
};

/// The files `clice format` formats without arguments: the build's
/// translation units that are the workspace's own (workspace_file: not in
/// a build tree, where FetchContent puts the sources it fetches) and
/// every file they include that is the workspace's own — inside it,
/// outside the cache directory, the build trees (the directories below the
/// root holding one of the build's databases: what a build generates or
/// fetches lives there) and the commands' system include directories, of
/// a type clang-format formats, no matching rule saying `format = false`.
std::vector<FormatFile> project_files(Project& project, llvm::ArrayRef<Fid> members) {
    auto& build = project.build;
    auto& files = project.file_table;

    CanonicalRef root = project.config.workspace_root;
    auto cache_dir = project.config.project.cache_dir.empty()
                         ? CanonicalPath()
                         : CanonicalPath(Spelling::absolute(project.config.project.cache_dir));
    std::vector<CanonicalPath> skipped_dirs;
    auto skip = [&](CanonicalPath dir) {
        if(!llvm::is_contained(skipped_dirs, dir)) {
            skipped_dirs.push_back(std::move(dir));
        }
    };
    if(!cache_dir.empty()) {
        skip(cache_dir);
    }
    // A database at the workspace root, or above it, is a copy of the
    // build's or the build of a larger tree, not a build tree of its own;
    // a root link to build/compile_commands.json names build/.
    for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
        CanonicalPath database(Spelling::absolute(project.cdb.source_path(SourceID(i))));
        auto directory = CanonicalRef(database).parent();
        if(directory != root && path::under(directory, root)) {
            skip(std::move(directory));
        }
    }
    for(auto member: members) {
        auto path = files.resolve(member);
        for(auto& command: build.commands(member)) {
            auto ref = build.resolve(member, command.config, command.source, path, path);
            auto search = project.cdb.search_config(ref);
            for(auto& dir: llvm::ArrayRef(search.dirs).drop_front(search.system_start_idx)) {
                skip(CanonicalPath(Spelling::absolute(dir.path)));
            }
        }
    }

    // A unit beside a database of its own directory, or in a directory its
    // own command searches as a system one, is still the project's: the
    // directories keep out only what units include.
    std::vector<FormatFile> result;
    llvm::DenseSet<Fid> unit(members.begin(), members.end());
    for(auto fid: project.dep_graph.all_files()) {
        auto path = files.resolve(fid);
        // A header only a command forces in, which no unit's text
        // includes, is left as it is: often a build generated it (CMake's
        // cmake_pch.hxx).
        if(!formats(path) || !build.formattable(path) ||
           (unit.contains(fid) ? !workspace_file(root, cache_dir, path)
                               : project.dep_graph.get_includers(fid).empty() ||
                                     llvm::any_of(skipped_dirs, [&](auto& dir) {
                                         return path::under(path, dir);
                                     }))) {
            continue;
        }
        result.push_back({.path = CanonicalPath(path), .name = files.spelling(fid)});
    }
    return result;
}

/// What formatting one file came to.
struct FileFormat {
    /// --check: where the edits fall, as `clang-format --dry-run --Werror`
    /// reports them; empty for a formatted file.
    std::string violations;

    /// Why the file could not be formatted; empty otherwise.
    std::string error;
};

std::string violations(const FormatFile& file,
                       llvm::StringRef content,
                       llvm::ArrayRef<TextReplacement> edits) {
    llvm::SourceMgr sources;
    sources.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBuffer(content, file.path, false),
                               llvm::SMLoc());
    std::string report;
    llvm::raw_string_ostream os(report);
    for(auto& edit: edits) {
        sources
            .GetMessage(llvm::SMLoc::getFromPointer(content.data() + edit.range.begin),
                        llvm::SourceMgr::DK_Error,
                        "code should be clang-formatted [-Wclang-format-violations]")
            .print(nullptr, os, /*ShowColors=*/false);
    }
    return report;
}

FileFormat format_file(const FormatFile& file, bool check) {
    auto content = vfs::read(file.path, vfs::Read::Bytes);
    if(!content) {
        return {.error = std::format("cannot read {}: {}", file.path, content.error().message())};
    }
    llvm::StringRef text = (*content)->getBuffer();
    auto edits = feature::document_format(file.name, text);
    if(!edits) {
        return {.error = std::move(edits.error())};
    }
    if(check) {
        return {.violations = violations(file, text, *edits)};
    }
    if(edits->empty()) {
        return {};
    }
    std::string formatted;
    std::uint32_t offset = 0;
    for(auto& edit: *edits) {
        formatted += text.slice(offset, edit.range.begin);
        formatted += edit.text;
        offset = edit.range.end;
    }
    formatted += text.substr(offset);
    if(auto error = vfs::write(file.path, formatted)) {
        return {.error = std::format("cannot write {}: {}", file.path, error.message())};
    }
    return {};
}

}  // namespace

BatchFormatResult run_batch_format(const BatchFormatOptions& options) {
    ScopedTimer timer;
    BatchFormatResult result;

    if(!vfs::is_directory(options.root)) {
        result.exit_code = 2;
        result.error = std::format("{}: not a directory", options.root);
        return result;
    }
    // A rewriting command does not run on defaults a broken configuration
    // fell back to: the exclusions would be gone with it.
    FileTable file_table;
    Project project{file_table};
    std::vector<ConfigIssue> issues;
    project.config = Config::load_from_workspace(options.root, &issues);
    for(auto& issue: issues) {
        if(issue.severity == ConfigIssue::Severity::Error) {
            result.exit_code = 2;
            result.error = std::format("{}: {}", issue.file, issue.message);
            return result;
        }
    }
    if(auto checked = check_requested_configuration(project.config, options.configuration);
       !checked) {
        result.exit_code = 2;
        result.error = std::move(checked.error());
        return result;
    }
    auto configuration = resolve_configuration(project.config, options.configuration);
    project.build.reset_active(configuration);

    // Explicit files are taken as given; explicit directories narrow the
    // build's own files to those under them, and only they need the build.
    std::vector<FormatFile> files;
    std::vector<CanonicalPath> directories;
    CanonicalRef root = project.config.workspace_root;
    for(auto& path: options.paths) {
        if(vfs::is_directory(path)) {
            directories.push_back(CanonicalPath(path));
        } else if(!vfs::exists(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: no such file", path);
            return result;
        } else if(!formats(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: not a C-family source file", path);
            return result;
        } else {
            // Rewritten where its bytes are, which have to be the
            // workspace's too.
            auto target = CanonicalPath(path);
            if(!path::under(target, root)) {
                if(path::under(CanonicalPath(path.parent()), root)) {
                    result.exit_code = 2;
                    result.error = std::format("{}: links outside the workspace", path);
                    return result;
                }
            } else if(project.build.formattable(target)) {
                files.push_back({.path = std::move(target), .name = path});
            }
        }
    }
    if(options.paths.empty() || !directories.empty()) {
        // A database that cannot be loaded makes the file set incomplete.
        auto load = load_build(project, options.root, configuration);
        if(project.cdb.source_count() == 0) {
            result.exit_code = 2;
            result.error = std::format(
                "Nothing to format: no compile_commands.json found in {} or its direct "
                "subdirectories.{}",
                options.root,
                deeper_databases_hint(project));
            return result;
        }
        for(std::size_t i = 0; i < project.cdb.source_count(); i += 1) {
            if(!project.cdb.loaded(SourceID(i))) {
                result.exit_code = 2;
                result.error = std::format("{}: the compilation database could not be loaded",
                                           project.cdb.source_path(SourceID(i)));
                return result;
            }
        }
        for(auto& file: project_files(project, load.members)) {
            if(!directories.empty() && llvm::none_of(directories, [&](auto& directory) {
                   return path::under(file.path, directory);
               })) {
                continue;
            }
            // A symlink into the workspace is the workspace's; one pointing
            // out of it is not.
            if(path::under(file.path, root)) {
                files.push_back(std::move(file));
            }
        }
    }
    // A file named and also under a named directory keeps the name given.
    std::ranges::stable_sort(files, {}, &FormatFile::path);
    auto duplicates = std::ranges::unique(files, {}, &FormatFile::path);
    files.erase(duplicates.begin(), duplicates.end());
    result.files = files.size();
    if(files.empty()) {
        result.seconds = timer.ms() / 1000.0;
        return result;
    }

    std::uint32_t jobs =
        options.jobs != 0 ? options.jobs : std::max(1u, std::thread::hardware_concurrency());
    std::vector<FileFormat> formats(files.size());
    std::atomic<std::size_t> next = 0;
    std::vector<llvm::thread> threads;
    for(std::uint32_t i = 0; i < jobs && i < files.size(); i += 1) {
        // As much stack as the editor's formatting gets on a worker's pool
        // thread.
        threads.emplace_back(std::optional<unsigned>(8u << 20), [&] {
            for(auto index = next.fetch_add(1); index < files.size(); index = next.fetch_add(1)) {
                formats[index] = format_file(files[index], options.check);
            }
        });
    }
    for(auto& thread: threads) {
        thread.join();
    }

    llvm::SetVector<llvm::StringRef> errors;
    for(auto [file, format]: llvm::zip_equal(files, formats)) {
        result.output += format.violations;
        if(!format.error.empty()) {
            errors.insert(format.error);
        } else if(!format.violations.empty()) {
            result.unformatted.push_back(file.path.str());
        }
    }
    if(!errors.empty()) {
        result.exit_code = 2;
        result.error = llvm::join(errors, "\n");
    } else if(!result.unformatted.empty()) {
        result.exit_code = 1;
    }
    result.seconds = timer.ms() / 1000.0;
    return result;
}

BatchResult run_batch_index(const BatchOptions& options) {
    kota::event_loop loop;
    BatchStack stack(loop);
    BatchResult result;
    loop.schedule(run(stack, options, result));
    loop.run();
    return result;
}

BatchLintResult run_batch_lint(const BatchLintOptions& options) {
    kota::event_loop loop;
    BatchStack stack(loop);
    BatchLintResult result;
    loop.schedule(run_lint(stack, options, result));
    loop.run();
    return result;
}

}  // namespace clice
