module clice:driver.run;

import :driver.driver;
import :feature.query;
import :index.query;
import :index.writer_lock;
import :project.background;
import :project.project;
import :server.control;
import :settings.settings;
import :support.logging;
import :support.prelude;
import :support.process;
import :support.version;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;
import :worker.protocol;

namespace clice::driver {

/// The root `where` names, found from `start`, and its configuration
/// (load_settings, `options` beside the configuration asked for): its
/// warnings logged, its errors the run's, each where it is, as compilers
/// name a place.
inline std::expected<std::pair<Spelling, LoadedSettings>, ProjectError>
    read_project(const ProjectOptions& where,
                 LoadSettingsOptions options = {},
                 const Spelling& start = Spelling::cwd()) {
    auto root = where.root(start);
    if(!root) {
        return std::unexpected(ProjectError{.message = std::move(root.error()), .exit_code = 2});
    }
    options.configuration = where.build.configuration;
    CanonicalPath canonical(*root);
    logging::set_project_roots({canonical.str()});
    auto loaded = load_settings(canonical, options);
    std::vector<std::string> errors;
    for(auto& issue: loaded.issues) {
        auto text = location(issue.file, issue.line, issue.column) + issue.message;
        if(issue.severity == SettingsIssue::Severity::Warning) {
            LOG_WARN("{}", text);
        } else {
            errors.push_back(std::move(text));
        }
    }
    if(!errors.empty()) {
        return std::unexpected(ProjectError{.message = llvm::join(errors, "\n"), .exit_code = 2});
    }
    return std::pair{std::move(*root), std::move(loaded)};
}

/// A project opened for a subcommand reading it: the project over a file
/// table of its own, which names files as the command line does.
struct OpenedProject {
    explicit OpenedProject(const Spelling& root) : project(files, CanonicalPath(root)) {
        files.spell_root(root);
    }

    FileTable files;

    Project project;

    /// What its opening found.
    Project::Opened opened;

    /// The units a --fresh refresh could not index, whose shards are as
    /// absent as a withheld file's.
    std::vector<std::string> failed;
};

/// The project at `root` under `loaded`, opened for a subcommand reading it
/// as `options` say, `configuration` set from `loaded`. `check_disk`: the
/// reads of its index withhold a file whose content moved on since it was
/// indexed, each file checked on disk once; off, they serve every file as it
/// was indexed.
inline std::unique_ptr<OpenedProject> open_at(const Spelling& root,
                                              LoadedSettings loaded,
                                              Project::OpenOptions options,
                                              bool check_disk = true) {
    auto opened = std::make_unique<OpenedProject>(root);
    auto& project = opened->project;
    project.settings = std::move(loaded.settings);
    options.configuration = loaded.configuration;
    options.freshness = check_disk ? index::FreshnessOptions{.check_disk = true}
                                   : index::FreshnessOptions{.withhold = false};
    opened->opened = project.open(options);
    return opened;
}

/// The project at `root` under `loaded` with its index, read-only, and
/// `load` of its build; why the index cannot be read is logged, and
/// `unusable` the error.
inline std::expected<std::unique_ptr<OpenedProject>, ProjectError>
    open_index(const Spelling& root,
               LoadedSettings loaded,
               Project::OpenOptions::Load load,
               std::string_view unusable,
               bool check_disk = true) {
    auto opened = open_at(root,
                          std::move(loaded),
                          {
                              .load = load,
                              .cache = Project::OpenOptions::Cache::Read,
                          },
                          check_disk);
    if(!opened->opened.unreadable.empty()) {
        LOG_ERROR("{}", opened->opened.unreadable);
        return std::unexpected(ProjectError{.message = std::string(unusable)});
    }
    return opened;
}

/// What the answers about symbols and files read of `project` (feature::query).
inline feature::query::Context query_context(const Project& project) {
    return {
        .query = project.index_query,
        .root = project.root,
        .cwd = Spelling::cwd(),
        .documentation = project.settings.documentation,
    };
}

/// What an answer lists as stale: `listed` and the files `project` withheld, sorted, once each.
inline std::vector<std::string> stale_files(const Project& project,
                                            std::vector<std::string> listed) {
    for(auto file: project.freshness.withheld()) {
        listed.emplace_back(project.file_table.display(file));
    }
    ranges::sort(listed);
    auto duplicates = ranges::unique(listed);
    listed.erase(duplicates.begin(), duplicates.end());
    return listed;
}

/// A snapshot of a batch index run's progress within the current pass:
/// units finished (indexed, skipped as fresh, or failed) against the
/// pass's total, and the files failed for good so far.
struct BatchProgress {
    std::size_t completed = 0;
    std::size_t total = 0;
    std::size_t failed = 0;
};

/// The project a batch run works on, as the command read it.
struct BatchProject {
    /// The project's root as the command line spells it; the result names
    /// files under this spelling.
    Spelling root;

    /// Its settings (load_settings), without errors.
    LoadedSettings loaded;

    /// Stateless worker count override; 0 runs one per logical core for
    /// indexing, the configuration's count for linting.
    std::uint32_t workers = 0;
};

struct BatchOptions {
    BatchProject project;

    /// Called on the event loop when a pass begins or ends and every ten
    /// seconds in between; null for no progress reporting.
    llvm::function_ref<void(const BatchProgress&)> on_progress;
};

/// What a batch indexing run did, for the driver's report. The run's own
/// story goes to the log; this carries only what the CLI prints and the
/// exit code encodes.
struct BatchResult {
    int exit_code = 0;

    /// A signal interrupted the run; the saved progress (all of it unless
    /// `unsaved`) lets a rerun resume.
    bool interrupted = false;

    /// The run reached its final summary (early failures skip it).
    bool completed = false;

    std::size_t indexed_tus = 0;

    std::size_t shard_count = 0;
    std::uint64_t shard_bytes = 0;
    std::size_t symbol_count = 0;

    /// Paths of the units that failed for good, sorted.
    std::vector<std::string> failed;

    /// A unit whose worker ran out of memory with no other work beside it,
    /// at `memory` bytes: it does not fit this machine.
    struct TooBig {
        std::string unit;

        std::uint64_t memory = 0;
    };

    /// The units too big for this machine, sorted by path.
    std::vector<TooBig> too_big;

    /// A unit indexed although its compile reported errors.
    struct UnitErrors {
        std::string unit;
        std::vector<worker::LocatedMessage> errors;
    };

    /// The units indexed although their compile reported errors, sorted
    /// by path: their index holds what parsed.
    std::vector<UnitErrors> compiled_with_errors;

    /// The session log directory; empty when file logging is off.
    std::string session_dir;

    /// Index state remained that the final save could not commit; a rerun
    /// cannot resume from it.
    bool unsaved = false;

    /// The index database was damaged: the final save discarded it, and
    /// the next run rebuilds the index.
    bool damaged = false;

    double seconds = 0;
};

/// One-shot batch indexing on the project's own builds — no sessions, no
/// transports: open the project, drain the background index, persist, and
/// wind down in contract-11 order. Runs its own event loop to completion.
/// Passes start immediately and indexing happens even when the settings
/// keep the background index disabled — running the command is the
/// request itself.
BatchResult run_batch_index(const BatchOptions& options);

/// What a batch run of `clice index` or `clice lint` works on: the
/// process's own file table and worker pool, and the project building over
/// them.
struct BatchRun {
    async::event_loop& loop;

    FileTable files;

    WorkerPool pool{loop};

    Project project{files, {}, &loop, &pool};

    /// The session log directory start_batch created; empty when file
    /// logging is off.
    std::string session_dir;

    explicit BatchRun(async::event_loop& loop) : loop(loop) {}
};

/// A path a worker reported, as the user knows the file; empty for none.
inline void show_path(FileTable& files, std::string& path) {
    if(!path.empty()) {
        path = files.display_identity(path);
    }
}

/// Batch startup: the command's settings with the run's overrides
/// (`workers` for the project's), the session file logger, and the worker
/// pool. `log_tag` names the run's log after the subcommand.
inline bool start_batch(BatchRun& run,
                        const BatchProject& batch,
                        std::uint32_t workers,
                        llvm::StringRef log_tag) {
    auto& project = run.project;
    run.files.spell_root(batch.root);
    project.root = CanonicalPath(batch.root);
    project.settings = batch.loaded.settings;
    auto& cfg = project.settings.project;
    cfg.idle_timeout_ms = 0;
    if(workers != 0) {
        cfg.stateless_worker_count = workers;
        cfg.min_stateless_worker_count = std::min(cfg.min_stateless_worker_count, workers);
        cfg.max_stateless_worker_count = workers;
    }

    logging::stderr_logger(log_tag);
    if(!cfg.logging_dir.empty()) {
        auto session_dir = logging::start_session_log(cfg.logging_dir);
        if(logging::file_logger(session_dir)) {
            LOG_INFO("clice {} ({}) runs {} on {}, pid {}",
                     clice::version,
                     clice::target,
                     log_tag,
                     batch.root,
                     sys::pid());
            if(auto source = built_from(); !source.empty()) {
                LOG_INFO("{}", source);
            }
            LOG_INFO("Session log directory: {}", session_dir);
            run.session_dir = std::move(session_dir);
        }
    }

    WorkerPoolOptions pool_opts;
    // Stateful workers host open documents; a batch run has none.
    pool_opts.stateful_count = 0;
    pool_opts.stateless_count = cfg.stateless_worker_count;
    pool_opts.min_stateless = cfg.min_stateless_worker_count;
    pool_opts.max_stateless = cfg.max_stateless_worker_count;
    pool_opts.session_dir = run.session_dir;
    if(!run.pool.start(pool_opts)) {
        LOG_ANOMALY(WorkerSpawnFail, "Failed to start worker pool");
        return false;
    }
    return true;
}

/// The shared contract-11 tail, then the pool.
inline async::task<> shutdown(BatchRun& run) {
    co_await run.project.shutdown();
    co_await run.pool.stop();
}

inline async::task<> checkpoints(BatchRun& run) {
    while(true) {
        // Time is the feature: the checkpoints' pace.
        co_await async::sleep(checkpoint_interval());
        co_await run.project.checkpoint();
    }
}

/// A batch run's signal handling and the order of its ending. Interruption
/// is judged only after the project shut down and the watchers finished: a
/// signal arriving while the final save/teardown ran must still report an
/// interruption, not a normal completion with exit code 0.
struct Lifetime {
    BatchRun& run;
    bool stop_requested = false;
    async::cancellation_source stop;
    async::task_group<> aux;

    explicit Lifetime(BatchRun& run) : run(run) {
        // Every signal stops gracefully: in-flight files are abandoned,
        // finished ones are persisted, and a rerun resumes from there. A
        // closed terminal sends SIGHUP.
        auto interrupt = [this] {
            LOG_INFO("Interrupted; saving indexing progress");
            stop.cancel();
        };
        aux.spawn(watch_termination(SIGINT, stop_requested, interrupt));
        aux.spawn(watch_termination(SIGTERM, stop_requested, interrupt));
        aux.spawn(watch_termination(sys::sighup, stop_requested, interrupt));
        aux.spawn(checkpoints(run));
        aux.spawn(run.files.disk.end_turns(run.loop));
    }

    /// Shuts the project down and stops the watchers; whether the run was
    /// interrupted.
    async::task<bool> finish() {
        co_await shutdown(run);
        aux.cancel();
        co_await aux.join();
        co_return stop_requested;
    }
};

/// How a run's index came up to date through the running server: the
/// server's pid and its answer.
struct ServerIndexed {
    std::uint32_t pid = 0;
    control::IndexResult result;
};

/// Bring the index of `project` up to date with the disk: through the
/// serving writer when a server holds the lock, else by a batch run of this
/// process, `on_progress` reporting it. Either runs a pass over the build
/// under the hash gate, so only units whose inputs changed are recompiled
/// — and an absent index gets built from nothing. An error when another
/// process holds the lock, or the server refuses.
inline std::expected<std::variant<ServerIndexed, BatchResult>, std::string>
    index_project(BatchProject project,
                  llvm::function_ref<void(const BatchProgress&)> on_progress) {
    auto& cache_dir = project.loaded.settings.project.cache_dir;
    auto writer = index::check_writer(cache_dir);
    switch(writer.state) {
        case index::WriterStatus::State::Server: {
            auto result = control::request_index(writer.endpoint, project.loaded.configuration);
            if(!result) {
                return std::unexpected(std::move(result.error()));
            }
            return ServerIndexed{.pid = writer.endpoint.pid, .result = std::move(*result)};
        }
        case index::WriterStatus::State::Held: {
            return std::unexpected(index::held_writer_message(writer, cache_dir));
        }
        case index::WriterStatus::State::Free: break;
    }
    return run_batch_index({.project = std::move(project), .on_progress = on_progress});
}

/// Bring the index up to date with the disk before a --fresh answer
/// (index_project). Returns the units that failed to index, or why the
/// refresh could not run.
inline std::expected<std::vector<std::string>, std::string> refresh(const Spelling& root,
                                                                    const LoadedSettings& loaded) {
    auto report_progress = [](const BatchProgress& progress) {
        driver::println(stderr,
                        "indexing {}/{} units, {} failed",
                        progress.completed,
                        progress.total,
                        progress.failed);
    };
    auto indexed = index_project({.root = root, .loaded = loaded}, report_progress);
    if(!indexed) {
        return std::unexpected(std::move(indexed.error()));
    }
    if(auto* server = std::get_if<ServerIndexed>(&*indexed)) {
        return std::move(server->result.failed);
    }
    auto& result = std::get<BatchResult>(*indexed);
    if(!result.completed) {
        return std::unexpected(result.interrupted ? "indexing interrupted"
                                                  : "indexing failed; see the log");
    }
    if(result.unsaved) {
        return std::unexpected("part of the index could not be persisted; see the log");
    }
    if(result.damaged) {
        return std::unexpected(
            "the index database is damaged (see the log); it is discarded, and the next `clice "
            "index` rebuilds the index");
    }
    return std::move(result.failed);
}

/// Open the project `where` names with its index, read-only, and `build`
/// of its build, brought up to date with the disk first under --fresh
/// (refresh). The error is the answer's.
inline std::expected<std::unique_ptr<OpenedProject>, ProjectError>
    open_project(const ProjectOptions& where,
                 const FreshOption& fresh,
                 Project::OpenOptions::Load load) {
    auto read = read_project(where);
    if(!read) {
        return std::unexpected(std::move(read.error()));
    }
    auto& [root, loaded] = *read;
    std::vector<std::string> failed;
    if(fresh.fresh) {
        auto refreshed = refresh(root, loaded);
        if(!refreshed) {
            return std::unexpected(ProjectError{.message = std::move(refreshed.error())});
        }
        failed = std::move(*refreshed);
    }
    auto opened = open_index(root,
                             std::move(loaded),
                             load,
                             "the index could not be opened (the log above says why); run "
                             "`clice index` or pass --fresh");
    if(opened) {
        (*opened)->failed = std::move(failed);
    }
    return opened;
}

/// The project `where` names with its index's tables bound, read-only, for
/// an analysis of the index. The error is the answer's.
inline std::expected<std::unique_ptr<OpenedProject>, ProjectError>
    open_for_analysis(const ProjectOptions& where) {
    auto read = read_project(where);
    if(!read) {
        return std::unexpected(std::move(read.error()));
    }
    auto& [root, loaded] = *read;
    return open_index(root,
                      std::move(loaded),
                      Project::OpenOptions::Load::Rules,
                      "no usable index; run `clice index` first");
}

}  // namespace clice::driver
