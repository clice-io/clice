#include "sched/batch.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <format>
#include <thread>

#include "command/command.h"
#include "config/config.h"
#include "sched/bootstrap.h"
#include "project/command_resolver.h"
#include "project/configuration.h"
#include "sched/families/pcm.h"
#include "sched/families/turun.h"
#include "sched/graph.h"
#include "sched/index/pump.h"
#include "project/index_store.h"
#include "project/project.h"
#include "support/anomaly.h"
#include "support/cache_store.h"
#include "support/filesystem.h"
#include "support/logging.h"
#include "support/timer.h"
#include "worker/pool.h"

#include "kota/async/async.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"

namespace clice {

namespace {

/// The lean batch assembly: the scheduling stack the server also runs on,
/// minus everything serving-side. No admission hooks — batch admits every
/// file.
struct BatchStack {
    kota::event_loop& loop;
    Workspace workspace;
    WorkerPool pool;
    CommandResolver commands{workspace};
    TaskGraph graph;
    PCMFamily pcm{graph, workspace, commands, pool};
    IndexStore store{loop, workspace, commands};
    TURunFamily turun{graph, workspace, commands, pcm, store, pool};
    IndexPump pump{loop, workspace, turun, store, pool};

    /// The session log directory start_batch created; empty when file
    /// logging is off.
    std::string log_dir;

    explicit BatchStack(kota::event_loop& loop) : loop(loop), pool(loop), graph(loop) {
        pcm.register_runner();
        turun.register_runner();
        pcm.on_indexing_needed = [this] {
            pump.schedule();
        };
    }
};

/// Poll until the pump has drained every round (requeue rounds included)
/// and persisted its results.
kota::task<> wait_until_indexed(const IndexPump& pump) {
    while(!pump.is_idle()) {
        co_await kota::sleep(200);
    }
}

/// The first signal asks for a graceful stop: in-flight files are
/// abandoned, finished ones are persisted, and a rerun resumes from
/// there. A second signal — of either watched kind, hence the shared
/// flag — exits immediately.
kota::task<> watch_signal(int signum, kota::cancellation_source& stop, bool& stop_requested) {
    auto watcher = kota::signal::create();
    if(!watcher || watcher->start(signum).has_error()) {
        co_return;
    }
    while(true) {
        co_await watcher->wait();
        if(stop_requested) {
            std::_Exit(130);
        }
        stop_requested = true;
        LOG_INFO("Interrupted; saving indexing progress");
        stop.cancel();
    }
}

/// Periodically checkpoint the cache store manifest (the store itself is
/// passive) and persist the index, so a crash on a long run loses at most
/// one interval of work: the pump saves only at round end, and a round
/// covers the whole workspace on a cold run.
kota::task<> checkpoint_task(BatchStack& stack) {
    constexpr auto interval = std::chrono::minutes(5);
    while(true) {
        co_await kota::sleep(interval);
        if(stack.workspace.store) {
            co_await kota::queue([&stack] { stack.workspace.store->checkpoint(); });
        }
        stack.pump.claim_report(co_await stack.store.save(stack.pump.save_debt()));
    }
}

/// The current round's progress to the driver's callback; nothing before
/// the first round has a total.
void report_progress(BatchStack& stack, const BatchOptions& options) {
    auto& round = stack.pump.progress();
    if(!options.on_progress || round.total == 0) {
        return;
    }
    options.on_progress(
        {.completed = round.completed, .total = round.total, .failed = stack.pump.failed().size()});
}

/// A paced report on top of the round boundaries: one unit can take
/// longer than the pace, and a run must not fall silent while it runs.
kota::task<> progress_ticker(BatchStack& stack, const BatchOptions& options) {
    constexpr auto pace = std::chrono::seconds(10);
    while(true) {
        co_await kota::sleep(pace);
        report_progress(stack, options);
    }
}

/// Quiesce the pump, then the shared contract-11 tail.
kota::task<> shutdown(BatchStack& stack) {
    co_await stack.pump.stop();
    co_await shutdown_indexing(stack.graph, stack.pump, stack.store, stack.pool, stack.workspace);
}

/// A batch run's signal handling and the order of its ending. Interruption
/// is judged only after the scheduling stack shut down and the watchers
/// settled: a signal arriving while the final save/teardown ran must still
/// report an interruption, not a normal completion with exit code 0.
struct BatchLifetime {
    BatchStack& stack;
    bool stop_requested = false;
    kota::cancellation_source stop;
    kota::task_group<> aux;

    explicit BatchLifetime(BatchStack& stack) : stack(stack), aux(stack.loop) {
        aux.spawn(watch_signal(SIGINT, stop, stop_requested));
        aux.spawn(watch_signal(SIGTERM, stop, stop_requested));
        aux.spawn(checkpoint_task(stack));
    }

    kota::cancellation_token token() {
        return stop.token();
    }

    /// Shuts the stack down and settles the watchers; whether the run was
    /// interrupted.
    kota::task<bool> finish() {
        co_await shutdown(stack);
        aux.cancel();
        co_await aux.join();
        co_return stop_requested;
    }
};

/// Shared batch startup: the finalized config with the run's overrides,
/// the session file logger, and the worker pool. `log_tag` names the log
/// files after the subcommand.
bool start_batch(BatchStack& stack,
                 llvm::StringRef root,
                 std::uint32_t workers,
                 llvm::StringRef self_path,
                 llvm::StringRef log_tag) {
    auto& workspace = stack.workspace;
    workspace.config = Config::load_from_workspace(root);
    auto& cfg = workspace.config.project;
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
    auto& workspace = stack.workspace;

    bool started = start_batch(stack, options.root, options.workers, options.self_path, "index");
    result.log_dir = stack.log_dir;
    if(!started) {
        result.exit_code = 1;
        // A failed later spawn leaves earlier workers and their I/O tasks
        // live; unstopped they keep the batch event loop spinning and the
        // command hangs instead of exiting.
        co_await stack.pool.stop();
        co_return;
    }
    if(!check_requested_configuration(workspace.config, options.configuration)) {
        result.exit_code = 1;
        co_await stack.pool.stop();
        co_return;
    }
    workspace.config.project.enable_indexing.value = true;

    auto report = bootstrap_workspace(workspace,
                                      stack.store,
                                      stack.pump,
                                      options.root,
                                      options.configuration,
                                      /*read_only_index=*/false,
                                      /*scan_tree=*/true);

    // The command's whole product is the persisted index: without storage
    // (cache failed to open, another process holds the index writer lock,
    // or an unreadable global blob disabled persistence) the run would
    // only warm this process's memory and a rerun would start from
    // nothing — fail instead of pretending.
    if(!workspace.index_db) {
        LOG_ERROR("Cannot persist the index at {}: the warning above says why; fix that and rerun",
                  std::string_view(workspace.config.project.cache_dir));
        result.exit_code = 1;
        co_await shutdown(stack);
        co_return;
    }
    if(report.members.empty()) {
        LOG_ERROR("Nothing to index: no compile_commands.json found under {}", options.root);
        result.exit_code = 1;
        co_await shutdown(stack);
        co_return;
    }

    auto progress = stack.pump.on_progress_changed.connect([&] {
        if(stack.pump.progress().stage != IndexPump::Progress::Stage::Report) {
            report_progress(stack, options);
        }
    });
    BatchLifetime lifetime(stack);
    lifetime.aux.spawn(progress_ticker(stack, options));
    co_await kota::with_token(wait_until_indexed(stack.pump), lifetime.token());
    if(co_await lifetime.finish()) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.indexed_tus = stack.pump.indexed_files();
    for(auto tu: llvm::make_first_range(workspace.project_index.manifests)) {
        if(!workspace.build.unit(tu)) {
            result.standalone_headers += 1;
        }
    }
    result.shard_count = workspace.project_index.shards.size();
    for(auto& shard: llvm::make_second_range(workspace.project_index.shards)) {
        result.shard_bytes += shard.bytes().size();
    }
    result.symbol_count = workspace.project_index.symbol_count();
    for(auto file: stack.pump.failed()) {
        result.failed.emplace_back(workspace.file_table.resolve(file));
    }
    std::ranges::sort(result.failed);
    // The shutdown save was the last retry for failed writes; whatever is
    // still dirty never reached disk and a rerun cannot resume from it.
    result.unsaved = stack.store.has_unsaved_state();
    if(!result.failed.empty() || result.unsaved) {
        result.exit_code = 1;
    }
    result.seconds = timer.ms() / 1000.0;
}

/// The lint sweep's shared counters and findings, living on run_lint's
/// frame.
struct LintSweep {
    std::size_t inflight = 0;
    std::size_t checked = 0;
    std::size_t failed = 0;
    std::vector<worker::TidyDiagnostic> findings;
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

kota::task<> lint_one(BatchStack& stack, bool with_index, Fid path_id, LintSweep& sweep) {
    auto file = stack.workspace.file_table.resolve(path_id);
    // A TU outside the lint set is here for the index only.
    TURunFamily::Plan plan;
    plan.tidy = stack.workspace.build.lintable(file);
    plan.index = with_index && stack.workspace.build.indexed(file);
    if(plan.tidy) {
        plan.tidy_params = tidy::resolve_tidy_params(file);
    }

    // One budget-free retry: a worker crash or preemption says nothing
    // about the TU, and a one-shot sweep has no later round to requeue
    // into.
    auto outcome = co_await stack.turun.run(path_id, plan);
    if(outcome.verdict == TURunFamily::Verdict::Crashed ||
       outcome.verdict == TURunFamily::Verdict::Preempted) {
        outcome = co_await stack.turun.run(path_id, std::move(plan));
    }

    switch(outcome.verdict) {
        case TURunFamily::Verdict::Completed: {
            if(plan.tidy) {
                sweep.checked += 1;
            }
            if(with_index) {
                stack.pump.claim_report(outcome.report);
            }
            // The lint set applies to the findings too: a header outside
            // it reports nothing. A compiler error stays wherever it is:
            // broken code is not a clean run.
            std::ranges::copy_if(outcome.tidy_diagnostics,
                                 std::back_inserter(sweep.findings),
                                 [&](const worker::TidyDiagnostic& d) {
                                     return d.check == "clang-diagnostic-error" ||
                                            stack.workspace.build.lintable(d.file);
                                 });
            break;
        }
        case TURunFamily::Verdict::Skipped:
        case TURunFamily::Verdict::Failed: {
            // Skipped means no real compile command (the index path keeps
            // last-known rows then; lint has nothing to keep) — either way
            // the TU went unchecked.
            sweep.failed += 1;
            LOG_WARN("Lint failed for {}: {}",
                     file,
                     outcome.error.empty() ? "no compile command found" : outcome.error);
            break;
        }
        case TURunFamily::Verdict::Crashed:
        case TURunFamily::Verdict::Preempted: {
            sweep.failed += 1;
            LOG_WARN("Lint gave up on {} after a retry: {}", file, outcome.error);
            break;
        }
        case TURunFamily::Verdict::Shutdown: {
            break;
        }
    }
    sweep.inflight -= 1;
    sweep.task_done.set();
}

kota::task<> run_lint_sweep(BatchStack& stack,
                            const BatchLintOptions& options,
                            llvm::ArrayRef<Fid> tus,
                            LintSweep& sweep) {
    kota::task_group<> workers(stack.loop);

    // The dispatch loop runs as a child of `workers`, like the pump's
    // round feeder: a cancel cascades through the join and every in-flight
    // task unwinds before the group is destroyed.
    auto feeder = [](BatchStack& stack,
                     const BatchLintOptions& options,
                     llvm::ArrayRef<Fid> tus,
                     LintSweep& sweep,
                     kota::task_group<>& workers) -> kota::task<> {
        for(auto path_id: tus) {
            // The pump feeder's window: deep enough that workers never
            // idle, shallow enough that a wind-down drains fast.
            while(sweep.inflight >=
                  std::max<std::size_t>(2 * stack.pool.effective_low_limit(), 2)) {
                sweep.task_done.reset();
                co_await sweep.task_done.wait();
            }
            sweep.inflight += 1;
            workers.spawn(lint_one(stack, options.with_index, path_id, sweep));
        }
    };
    workers.spawn(feeder(stack, options, tus, sweep, workers));
    co_await workers.join();
}

kota::task<> run_lint(BatchStack& stack, const BatchLintOptions& options, BatchLintResult& result) {
    ScopedTimer timer;
    auto& workspace = stack.workspace;

    if(!start_batch(stack, options.root, options.workers, options.self_path, "lint")) {
        result.exit_code = 2;
        // See run(): stop the partially started pool or the loop never
        // drains.
        co_await stack.pool.stop();
        co_return;
    }
    // The command's product is the lint report: the background sweep must
    // not race the plan's own runs, and without --index nothing may touch
    // the persisted index — the read-only load queues no reconciliation
    // or sweep writes, so the shutdown save commits nothing.
    if(!check_requested_configuration(workspace.config, options.configuration)) {
        result.exit_code = 2;
        co_await stack.pool.stop();
        co_return;
    }
    workspace.config.project.enable_indexing.value = false;

    auto report = bootstrap_workspace(workspace,
                                      stack.store,
                                      stack.pump,
                                      options.root,
                                      options.configuration,
                                      /*read_only_index=*/!options.with_index,
                                      /*scan_tree=*/true);

    auto& members = report.members;
    if(members.empty()) {
        LOG_ERROR("Nothing to lint: no compile_commands.json found under {}", options.root);
        result.exit_code = 2;
        co_await shutdown(stack);
        co_return;
    }
    if(options.with_index && !workspace.index_db) {
        LOG_ERROR("Cannot persist the index at {}; see the log for the cause and rerun",
                  std::string_view(workspace.config.project.cache_dir));
        result.exit_code = 2;
        co_await shutdown(stack);
        co_return;
    }

    // One run per file: a file with several CDB entries lints once, under
    // the command resolve_command picks — same as the indexing sweep. A TU
    // the rules keep out of the lint set is not even parsed, unless the
    // index wants it.
    llvm::SmallVector<Fid> tus;
    for(auto member: members) {
        auto file = workspace.file_table.resolve(member);
        if(workspace.build.lintable(file) ||
           (options.with_index && workspace.build.indexed(file))) {
            tus.push_back(member);
        }
    }

    BatchLifetime lifetime(stack);
    LintSweep sweep;
    co_await kota::with_token(run_lint_sweep(stack, options, tus, sweep), lifetime.token());
    // What landed is the report, whole or cut short by an interruption.
    merge_findings(sweep.findings);
    result.findings = std::move(sweep.findings);
    if(options.with_index && !lifetime.stop_requested) {
        // The sweep's merges can owe other TUs a reindex (a rebuilt shared
        // shard dropped their variants), and bootstrap may have claimed
        // prior-session debt — the sweep runs outside the pump, so nothing
        // settled any of it. Drain it through the pump like the index
        // batch does, or an already-linted owner's rows stay missing while
        // the run exits clean.
        workspace.config.project.enable_indexing.value = true;
        stack.pump.schedule(/*immediate=*/true);
        co_await kota::with_token(wait_until_indexed(stack.pump), lifetime.token());
    }
    if(co_await lifetime.finish()) {
        result.interrupted = true;
        result.exit_code = 130;
        co_return;
    }

    result.completed = true;
    result.checked_tus = sweep.checked;
    result.failed_tus = sweep.failed + stack.pump.failed().size();
    // The shutdown save was the last retry: with --index the persisted
    // index is part of the product, so unsaved state must fail the run
    // like the index-only batch does.
    result.unsaved = options.with_index && stack.store.has_unsaved_state();
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

/// The files `clice format` formats without arguments: the build's
/// translation units and every file they include that is the workspace's
/// own — inside it, outside the cache directory, the build trees (the
/// directories below the root holding one of `databases`: what a build
/// generates or fetches lives there) and the commands' system include
/// directories, of a type clang-format formats, no matching rule saying
/// `format = false`.
std::vector<std::string> project_files(Workspace& workspace,
                                       llvm::ArrayRef<Fid> members,
                                       llvm::ArrayRef<std::string> databases) {
    auto& build = workspace.build;
    auto& files = workspace.file_table;

    llvm::StringRef root = workspace.config.workspace_root;
    llvm::StringSet<> skipped_dirs;
    skipped_dirs.insert(build.as_configured(workspace.config.project.cache_dir));
    // A database at the workspace root, or above it, is a copy of the
    // build's or the build of a larger tree, not a build tree of its own.
    for(auto& database: databases) {
        auto directory = build.as_configured(path::parent_path(database));
        if(directory != root && path::under(directory, root)) {
            skipped_dirs.insert(directory);
        }
    }
    for(auto member: members) {
        auto path = files.resolve(member);
        for(auto& command: build.commands(member)) {
            auto ref =
                build.resolve(member, command.config, command.source, llvm::StringRef(path), path);
            auto search = workspace.cdb.search_config(ref);
            for(auto& dir: llvm::ArrayRef(search.dirs).drop_front(search.system_start_idx)) {
                skipped_dirs.insert(build.as_configured(dir.path));
            }
        }
    }

    // The build's own units are its own wherever they sit; the
    // directories keep out only what they include.
    std::vector<std::string> result;
    llvm::DenseSet<Fid> unit(members.begin(), members.end());
    for(auto fid: workspace.dep_graph.all_files()) {
        auto path = build.as_configured(files.resolve(fid));
        if(!formats(path) || !build.formattable(path) ||
           (!unit.contains(fid) && llvm::any_of(skipped_dirs, [&](auto& dir) {
               return path::under(path, dir.getKey());
           }))) {
            continue;
        }
        result.push_back(std::move(path));
    }
    return result;
}

/// Where a file's bytes are: clang-format's in-place mode replaces a
/// symlink with a regular file, so the target is what it must be given.
std::string physical(llvm::StringRef path) {
    llvm::SmallString<256> real;
    if(llvm::sys::fs::real_path(path, real)) {
        return path.str();
    }
    std::string result(real);
    path::canonicalize(result);
    return result;
}

/// The stderr of one clang-format run; `status` is negative when the
/// process could not be run or waited for, `error` saying why.
struct ToolRun {
    std::int64_t status = -1;
    std::string output;
    std::string error;
};

kota::task<std::string> drain_pipe(kota::pipe pipe) {
    std::string buffer;
    while(true) {
        auto chunk = co_await pipe.read();
        if(!chunk.has_value() || chunk.value().empty()) {
            break;
        }
        buffer += chunk.value();
    }
    co_return buffer;
}

kota::task<ToolRun> run_clang_format(kota::event_loop& loop,
                                     const std::string& executable,
                                     bool check,
                                     llvm::ArrayRef<std::string> chunk) {
    kota::process::options opts;
    opts.file = executable;
    opts.args = {executable};
    if(check) {
        opts.args.push_back("--dry-run");
        opts.args.push_back("--Werror");
    } else {
        opts.args.push_back("-i");
    }
    opts.args.insert(opts.args.end(), chunk.begin(), chunk.end());
    opts.streams = {
        kota::process::stdio::ignore(),
        kota::process::stdio::ignore(),
        kota::process::stdio::pipe(false, true),
    };
    auto spawn = kota::process::spawn(opts, loop);
    if(!spawn.has_value()) {
        co_return ToolRun{
            .error = std::format("cannot run {}: {}", executable, spawn.error().message())};
    }
    auto& child = *spawn;
    auto output = co_await drain_pipe(std::move(child.stderr_pipe));
    auto exit = co_await child.proc.wait();
    if(!exit.has_value()) {
        co_return ToolRun{
            .output = std::move(output),
            .error =
                std::format("{} did not exit cleanly: {}", executable, exit.error().message())};
    }
    if(exit->term_signal != 0) {
        co_return ToolRun{
            .output = std::move(output),
            .error = std::format("{} was killed by signal {}", executable, exit->term_signal)};
    }
    co_return ToolRun{.status = exit->status, .output = std::move(output)};
}

/// One of the `jobs` workers: runs the next unclaimed chunk until none is
/// left.
kota::task<> format_chunks(kota::event_loop& loop,
                           const std::string& executable,
                           bool check,
                           llvm::ArrayRef<std::vector<std::string>> chunks,
                           std::size_t& next,
                           std::vector<ToolRun>& runs) {
    while(next < chunks.size()) {
        auto index = next;
        next += 1;
        runs[index] = co_await run_clang_format(loop, executable, check, chunks[index]);
    }
}

kota::task<> run_format_sweep(kota::event_loop& loop,
                              const std::string& executable,
                              bool check,
                              std::uint32_t jobs,
                              llvm::ArrayRef<std::vector<std::string>> chunks,
                              std::vector<ToolRun>& runs) {
    std::size_t next = 0;
    kota::task_group<> workers(loop);
    for(std::uint32_t i = 0; i < jobs && i < chunks.size(); i += 1) {
        workers.spawn(format_chunks(loop, executable, check, chunks, next, runs));
    }
    co_await workers.join();
}

/// The file a `--dry-run --Werror` violation names:
/// `<file>:<line>:<col>: error: code should be clang-formatted [...]`.
std::optional<llvm::StringRef> violation_file(llvm::StringRef line) {
    if(!line.contains("[-Wclang-format-violations]")) {
        return std::nullopt;
    }
    auto location = line.split(": error:").first;
    return location.rsplit(':').first.rsplit(':').first;
}

}  // namespace

BatchFormatResult run_batch_format(const BatchFormatOptions& options) {
    ScopedTimer timer;
    BatchFormatResult result;

    // A name is looked up in PATH; a path is taken as given, and either
    // way the file must exist to be run.
    auto executable = llvm::sys::findProgramByName(options.clang_format);
    if(!executable || !llvm::sys::fs::can_execute(*executable)) {
        result.exit_code = 2;
        result.error = std::format("clang-format not found ({}): install it or pass --clang-format",
                                   options.clang_format);
        return result;
    }

    if(!llvm::sys::fs::is_directory(options.root)) {
        result.exit_code = 2;
        result.error = std::format("{}: not a directory", options.root);
        return result;
    }
    // A rewriting command does not run on defaults a broken configuration
    // fell back to: the exclusions would be gone with it.
    Workspace workspace;
    std::vector<ConfigIssue> issues;
    workspace.config = Config::load_from_workspace(options.root, &issues);
    for(auto& issue: issues) {
        if(issue.severity == ConfigIssue::Severity::Error) {
            result.exit_code = 2;
            result.error = std::format("{}: {}", issue.file, issue.message);
            return result;
        }
    }
    if(!check_requested_configuration(workspace.config, options.configuration)) {
        result.exit_code = 2;
        result.error = "the requested configuration does not exist";
        return result;
    }
    auto configuration = resolve_configuration(workspace.config, options.configuration);
    workspace.build.reset_active(configuration);

    // Explicit files are taken as given; explicit directories narrow the
    // build's own files to those under them, and only they need the build.
    std::vector<std::string> files;
    llvm::SmallVector<llvm::StringRef> directories;
    for(auto& path: options.paths) {
        if(llvm::sys::fs::is_directory(path)) {
            directories.push_back(path);
        } else if(!llvm::sys::fs::exists(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: no such file", path);
            return result;
        } else if(!formats(path)) {
            result.exit_code = 2;
            result.error = std::format("{}: not a C-family source file", path);
            return result;
        } else if(workspace.build.formattable(path)) {
            // Rewritten where its bytes are, which has to be the
            // workspace's too.
            auto target = physical(path);
            if(!path::under(target, workspace.config.workspace_root) &&
               !path::under(target, workspace.config.workspace_real_root)) {
                result.exit_code = 2;
                result.error = std::format("{}: links outside the workspace", path);
                return result;
            }
            files.push_back(std::move(target));
        }
    }
    if(options.paths.empty() || !directories.empty()) {
        // Like the other batch commands, which open no file: every
        // database under the workspace applies when no rule names one,
        // and one that cannot be loaded makes the file set incomplete.
        llvm::SmallVector<std::string> databases;
        for(auto declared: workspace.build.declared_sources()) {
            databases.push_back(declared.str());
        }
        if(databases.empty()) {
            databases = compile_commands_below(options.root, workspace.config.project.cache_dir);
        }
        auto load = load_build(workspace, options.root, configuration, databases);
        for(auto& database: databases) {
            auto id = workspace.cdb.find_source(database);
            if(!id || !workspace.cdb.loaded(*id)) {
                result.exit_code = 2;
                result.error =
                    std::format("{}: the compilation database could not be loaded", database);
                return result;
            }
        }
        llvm::StringRef root = workspace.config.workspace_root;
        llvm::StringRef real_root = workspace.config.workspace_real_root;
        for(auto& path: project_files(workspace, load.members, databases)) {
            if(!directories.empty() && llvm::none_of(directories, [&](llvm::StringRef directory) {
                   return path::under(path, directory);
               })) {
                continue;
            }
            // A symlink into the workspace is the workspace's; one pointing
            // out of it is not.
            auto target = physical(path);
            if(path::under(target, root) || path::under(target, real_root)) {
                files.push_back(std::move(target));
            }
        }
    }
    std::ranges::sort(files);
    auto duplicates = std::ranges::unique(files);
    files.erase(duplicates.begin(), duplicates.end());
    result.files = files.size();
    if(files.empty()) {
        result.seconds = timer.ms() / 1000.0;
        return result;
    }

    // One process per chunk: clang-format takes many files at once, and
    // the command line stays under the limit Windows imposes.
    constexpr std::size_t chunk_bytes = 24 * 1024;
    constexpr std::size_t chunk_files = 64;
    std::vector<std::vector<std::string>> chunks;
    std::size_t bytes = 0;
    for(auto& file: files) {
        if(chunks.empty() || chunks.back().size() == chunk_files ||
           bytes + file.size() > chunk_bytes) {
            chunks.emplace_back();
            bytes = 0;
        }
        bytes += file.size() + 1;
        chunks.back().push_back(file);
    }

    std::uint32_t jobs =
        options.jobs != 0 ? options.jobs : std::max(1u, std::thread::hardware_concurrency());
    std::vector<ToolRun> runs(chunks.size());
    kota::event_loop loop;
    loop.schedule(run_format_sweep(loop, *executable, options.check, jobs, chunks, runs));
    loop.run();

    llvm::StringSet<> unformatted;
    bool failed = false;
    for(auto& run: runs) {
        result.output += run.output;
        if(!run.error.empty()) {
            result.error = run.error;
            failed = true;
            continue;
        }
        if(options.check) {
            llvm::SmallVector<llvm::StringRef> lines;
            llvm::StringRef(run.output).split(lines, '\n', -1, /*KeepEmpty=*/false);
            std::size_t violations = 0;
            bool errors = false;
            for(auto line: lines) {
                if(auto file = violation_file(line)) {
                    unformatted.insert(*file);
                    violations += 1;
                } else {
                    errors |= line.contains(": error:") || line.starts_with("error:") ||
                              line.starts_with("Error");
                }
            }
            // --Werror exits 1 for violations too: a non-zero status is a
            // real failure when none is reported, or when an error of
            // clang-format's own (a file it cannot read, a configuration it
            // cannot parse) sits next to them.
            failed |= run.status != 0 && (violations == 0 || errors);
        } else {
            failed |= run.status != 0;
        }
    }
    for(auto& file: unformatted) {
        result.unformatted.emplace_back(file.getKey());
    }
    std::ranges::sort(result.unformatted);
    if(failed) {
        result.exit_code = 2;
        if(result.error.empty()) {
            result.error = std::format("{} failed; see its output above", *executable);
        }
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
