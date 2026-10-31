module;

#include <errno.h>

module clice:driver.driver;

import :feature.query;
import :index.query;
import :index.writer_lock;
import :project.discovery;
import :project.indexer;
import :project.project;
import :server.control;
import :settings.settings;
import :support.anomaly;
import :support.json;
import :support.logging;
import :support.prelude;
import :support.process;
import :vfs.cache_store;
import :vfs.file_table;
import :vfs.path;
import :worker.pool;
import :worker.protocol;

namespace clice::driver {

/// The subcommands, in the order `clice --help` lists them, each named by
/// its enumerator.
enum class Command : std::uint8_t {
    serve,
    query,
    refactor,
    index,
    lint,
    format,
    inspect,
    command,
    scan,
    analyze,
    modularize,
};

/// Runs a subcommand on the arguments after its name; returns the process
/// exit code.
using CommandHandler = kotatsu::function<int(std::span<std::string>)>;

/// A subcommand as its file registers it: its line of `clice --help`, and
/// what makes its handler.
struct CommandEntry {
    std::string_view description;
    CommandHandler (*build)() = nullptr;
};

inline auto& command_entries() {
    static std::array<CommandEntry, meta::reflection<Command>::member_count> entries;
    return entries;
}

/// Register a subcommand. Its file calls this from the initializer of an
/// ordinary variable at namespace scope: the linker's garbage collection
/// (--gc-sections, /OPT:REF, LTO) may drop a variable no one reads, with
/// its initializer, when it is a COMDAT of its own, as an inline variable
/// or a template's static member is, but never an ordinary one. Nothing
/// names a subcommand's file either, so the driver library is linked whole
/// (alwayslink).
inline bool register_command(Command command, CommandEntry entry) {
    auto& registered = command_entries()[std::to_underlying(command)];
    assert(!registered.build && "a subcommand registers once");
    registered = entry;
    return true;
}

/// `clice worker`, which the master spawns its workers as: no command of a
/// user's, so no Command and no line of `clice --help`.
int run_worker(std::span<std::string> args);

/// Add every subcommand to `root`, in the order of Command. One whose file
/// did not register it stops the program.
inline void add_commands(deco::cli::SubCommander& root) {
    for(auto command: meta::reflection<Command>::member_values) {
        auto name = meta::enum_name(command);
        auto& entry = command_entries()[std::to_underlying(command)];
        if(!entry.build) {
            logging::critical("The {} subcommand is not registered", name);
        }
        root.add({.name = name, .description = entry.description}, entry.build());
    }
}

/// Write command output. stdout carries the command's answer. On a pipe
/// or a socket, a write fails only because the reader went away (`clice …
/// | head`), under whatever error the kernel picks for the descriptor and
/// the timing (EPIPE, ECONNRESET, ENOTCONN on macOS, EINVAL on Windows),
/// and the process ends as a command-line tool does there: by SIGPIPE,
/// which clice otherwise ignores (a dead worker's pipe must not take the
/// master down), or on Windows, which has none, quietly with 141, the
/// status a POSIX shell reports for it. The exception is EAGAIN: a
/// non-blocking pipe, shared from a parent's own, refuses while its reader
/// lags. That and any other failure (a full disk under a redirect) are
/// reported and exit 1. stderr carries progress and complaints, which,
/// like log lines, are dropped when they cannot be written: a progress
/// line must not end a run before it saves.
inline void write_output(std::FILE* stream, std::string_view text) {
    int fd = ::fileno(stream);
    if(sys::is_console(fd) && !sys::write_console(fd, text)) {
        return;
    }
    if(std::fwrite(text.data(), 1, text.size(), stream) == text.size() &&
       std::fflush(stream) == 0) {
        return;
    }
    if(stream == stderr) {
        return;
    }
    int error = errno;
    if(error != EAGAIN && sys::is_pipe(fd)) {
#ifdef _WIN32
        std::_Exit(141);
#else
        std::signal(SIGPIPE, SIG_DFL);
        std::raise(SIGPIPE);
#endif
    }
    LOG_ERROR("Cannot write the output: {}",
              std::error_code(error, std::generic_category()).message());
    std::_Exit(1);
}

/// std::println through write_output; calls stay qualified
/// (`driver::println`), or argument-dependent lookup also finds std's.
template <typename... Args>
void println(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
    auto text = std::format(fmt, std::forward<Args>(args)...);
    text.push_back('\n');
    write_output(stream, text);
}

template <typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    driver::println(stdout, fmt, std::forward<Args>(args)...);
}

inline void println() {
    write_output(stdout, "\n");
}

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

/// The --log-level option every subcommand's options hold: what reaches
/// stderr. A session log keeps info whatever stderr shows, as an
/// info-level session log is what tells what a run did.
struct LogLevelOption {
    DecoKV(help = "Log level of stderr", required = false)
    <LogLevel> log_level = LogLevel::Warn;

    /// Set the global levels: stderr's as given, the logs' info at least.
    void apply() const {
        auto level = [&] {
            switch(*log_level) {
                case LogLevel::Trace: return logging::Level::trace;
                case LogLevel::Debug: return logging::Level::debug;
                case LogLevel::Info: return logging::Level::info;
                case LogLevel::Warn: return logging::Level::warn;
                case LogLevel::Error: return logging::Level::err;
                case LogLevel::Off: return logging::Level::off;
            }
            std::unreachable();
        }();
        logging::options.stderr_level = level;
        logging::options.level = std::min(level, logging::Level::info);
    }
};

/// --configuration: the build configuration a subcommand works in.
struct ConfigurationOption {
    DecoKV(help =
               "Build configuration, one of the tags declared on rules (default: the selected "
               "one, else default_configuration)",
           required = false)
    <std::string> configuration;

    /// The tag given, empty for none.
    std::string tag() const {
        return configuration.value_or("");
    }
};

/// The project a subcommand works on, and its build configuration.
struct ProjectOptions {
    DecoKV(help =
               "Project root directory (default: the nearest directory at or above the current "
               "one holding a clice.toml or a compile_commands.json)",
           required = false)
    <std::string> workspace;

    ConfigurationOption configuration;

    /// The project's root as spelled: --workspace as given, relative to
    /// the current directory; else the project root at or above `start`
    /// (find_project_root), else `start`. An error for a --workspace that
    /// is no directory.
    std::expected<Spelling, std::string> root(const Spelling& start = Spelling::cwd()) const {
        if(workspace) {
            Spelling root(*workspace, Spelling::cwd());
            if(!fs::is_directory(root)) {
                return std::unexpected(std::format("{}: not a directory", root));
            }
            return root;
        }
        auto root = find_project_root(start);
        return root.empty() ? start : root;
    }
};

/// How many units or files a batch subcommand works on at once.
struct JobsOption {
    DecoKV(names = {"-j", "--jobs"},
           help =
               "How many units or files to work on at once (default: "
               "project.stateless_worker_count; for index and format, the CPU count)",
           required = false)
    <std::uint32_t> jobs;
};

/// --json, of the batch subcommands that report a run.
struct JsonReportOption {
    DecoFlag(names = {"--json"},
             help = "Print the run's report as JSON on stdout instead of text",
             required = false)
    json;
};

/// Where a message is, as compilers print it; nothing for one no file
/// holds, the file alone for one about its whole compile.
inline std::string location(llvm::StringRef file, std::uint32_t line, std::uint32_t column) {
    if(file.empty()) {
        return {};
    }
    if(line == 0) {
        return std::format("{}: ", file);
    }
    return std::format("{}:{}:{}: ", file, line, column);
}

/// The file an operation is about.
struct FileTarget {
    DecoInput(meta_var = "<PATH>", help = "The file, relative to the current directory or absolute")
    <std::string> path;
};

/// The symbol an operation is about.
struct SymbolTarget {
    DecoInput(meta_var = "<LOCATOR>",
              help = "The symbol, as a locator: a name, an answer's symbol, or file:line[:column]")
    <std::string> locator;
};

/// --fresh, of the subcommands that read the index.
struct FreshOption {
    DecoFlag(names = {"--fresh"},
             help =
                 "Reindex the files whose content changed since they were indexed first, "
                 "through the running server or a batch run of this command",
             required = false)
    fresh;
};

template <typename Parser>
void print_usage(Parser& cmd) {
    std::ostringstream ss;
    cmd.usage(ss);
    write_output(stdout, ss.str());
}

/// "s" when `count` warrants a plural noun, for user-facing summaries.
const inline char* plural_s(std::size_t count) {
    return count == 1 ? "" : "s";
}

/// One answer on stdout: `{"result": ..., "stale": [...]}`, the files
/// whose shards were withheld because their content moved on from the
/// index (or that the index never held) listed so the reader knows what
/// the answer lacks.
template <typename T>
struct Answer {
    T result;
    std::vector<std::string> stale;
};

/// `{"error": "...", "stale": [...]}` on stdout, exit code 1 (2 for
/// arguments that do not parse): what kept the question from being
/// answered, and the files withheld on the way — a symbol not found may
/// sit in one of them. A locator naming several symbols where one was
/// wanted lists them.
struct Failure {
    std::string error;
    protocol::optional<std::vector<feature::query::SymbolEntry>> candidates;
    std::vector<std::string> stale;
};

template <typename T>
std::string render_json(const T& value) {
    return to_client_json(value, "null");
}

template <typename T>
void print_json(const T& value) {
    driver::println("{}", render_json(value));
}

/// Answer that a subcommand answering in JSON could not do what it was
/// asked; returns its exit code.
inline int fail(std::string error) {
    print_json(Failure{.error = std::move(error)});
    return 1;
}

/// How a subcommand reports arguments it cannot take: as text on stderr,
/// or as the Failure on stdout of one that answers in JSON.
enum class Errors : std::uint8_t { Text, JSON };

/// Report arguments the subcommand cannot take; returns the exit code of
/// arguments that do not parse.
inline int usage_error(Errors errors, std::string message) {
    if(errors == Errors::JSON) {
        print_json(Failure{.error = std::move(message)});
    } else {
        driver::println(stderr, "{}", message);
    }
    return deco::cli::parse_error_exit_code;
}

/// A subcommand, or one operation of it, over the options `Options`, whose
/// LogLevelOption is `log`: it applies the level, logs to stderr as
/// `logger` and returns what `run` does with the options.
template <typename Options, typename Run>
CommandHandler operation(std::string_view usage, std::string_view logger, Errors errors, Run run) {
    auto command = deco::cli::command<Options>(usage);
    command.match_all([logger, run = std::move(run)](Options opts) mutable {
        opts.log.apply();
        logging::stderr_logger(logger, logging::options);
        return run(opts);
    });
    if(errors == Errors::JSON) {
        command.on_error([](auto err) { print_json(Failure{.error = err.message}); });
    }
    return command;
}

/// The error handler of a commander of subcommands: named without one, it
/// prints its usage and succeeds, as --help does; an unknown subcommand is
/// a usage error.
inline auto subcommand_error_handler(deco::cli::SubCommander& commander,
                                     Errors errors = Errors::Text) {
    return [&commander, errors](const deco::cli::SubCommandError& err) {
        if(err.type == deco::cli::SubCommandError::Type::MissingSubCommand) {
            print_usage(commander);
            return 0;
        }
        return usage_error(errors, err.message);
    };
}

/// Run a commander of subcommands on `args`. The commander takes whatever
/// leads them for a subcommand's name; an option other than --help leading
/// them is reported as the unknown option it is.
inline int run_subcommands(deco::cli::SubCommander& commander,
                           std::span<std::string> args,
                           Errors errors = Errors::Text) {
    namespace text = deco::cli::text;
    if(!args.empty() && args.front().starts_with("-") &&
       !ranges::contains(deco::decl::help_names, args.front())) {
        return usage_error(
            errors,
            text::render_diagnostic(
                text::diagnostic_at(args, 0, 1, std::format("unknown option '{}'", args.front()))));
    }
    return commander(args);
}

/// An operation of a subcommand: the word naming it, its line of the
/// subcommand's help, and what runs it on the arguments after the word.
struct Operation {
    std::string_view name;
    std::string_view description;
    CommandHandler run;
};

/// A subcommand whose first argument names one of its `operations`. With
/// `main`, the subcommand's own job, arguments naming none (none at all,
/// options, or with `main_targets` the targets the job takes) run it, and
/// its help lists the operations after its own. Without, no arguments
/// print the operations, which `overview` heads, and an option before an
/// operation is an unknown one.
inline CommandHandler with_operations(std::string_view overview,
                                      std::vector<Operation> operations,
                                      Errors errors,
                                      std::optional<CommandHandler> main = std::nullopt,
                                      bool main_targets = false) {
    auto commander = std::make_shared<deco::cli::SubCommander>("", main ? "" : overview);
    std::vector<std::string> names;
    for(auto& operation: operations) {
        names.emplace_back(operation.name);
        commander->add({.name = operation.name, .description = operation.description},
                       std::move(operation.run));
    }
    commander->when_err(subcommand_error_handler(*commander, errors));
    if(!main) {
        commander->enable_help();
        return [commander, errors](std::span<std::string> args) {
            return run_subcommands(*commander, args, errors);
        };
    }
    return [commander, main = std::move(*main), names = std::move(names), main_targets](
               std::span<std::string> args) mutable {
        if(!args.empty() && !args.front().starts_with("-") &&
           (!main_targets || ranges::contains(names, args.front()))) {
            return (*commander)(args);
        }
        auto code = main(args);
        // Arguments that do not parse fail before --help is seen.
        if(code == 0 && ranges::any_of(args, [](std::string_view arg) {
               return ranges::contains(deco::decl::help_names, arg);
           })) {
            driver::println();
            print_usage(*commander);
        }
        return code;
    };
}

/// Why a subcommand cannot work on its project, and the exit code it ends
/// with: 2 for arguments or a configuration it cannot take, 1 otherwise.
struct ProjectError {
    std::string message;
    int exit_code = 1;
};

/// Answer that a subcommand answering in JSON cannot work on its project;
/// returns its exit code.
inline int fail(ProjectError error) {
    print_json(Failure{.error = std::move(error.message)});
    return error.exit_code;
}

/// Tell on stderr why a subcommand answering in text cannot work on its
/// project; returns its exit code.
inline int print_error(const ProjectError& error) {
    LOG_ERROR("{}", error.message);
    return error.exit_code;
}

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
    auto configuration = where.configuration.tag();
    options.configuration = configuration;
    auto loaded = load_settings(CanonicalPath(*root), options);
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
    std::string log_dir;

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
    std::string log_dir;

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
/// pool. `log_tag` names the log files after the subcommand.
inline bool start_batch(BatchRun& run,
                        const BatchProject& batch,
                        std::uint32_t workers,
                        llvm::StringRef log_tag) {
    auto& project = run.project;
    run.files.spell_root(batch.root);
    project.root = CanonicalPath(batch.root);
    project.settings = batch.loaded.settings;
    auto& cfg = project.settings.project;
    cfg.idle_timeout_ms.value = 0;
    if(workers != 0) {
        cfg.stateless_worker_count.value = workers;
        cfg.min_stateless_worker_count.value =
            std::min(cfg.min_stateless_worker_count.value, workers);
        cfg.max_stateless_worker_count.value = workers;
    }

    if(cfg.cache_dir_defaulted.value) {
        CacheStore::write_ignore_markers(cfg.cache_dir);
    }

    if(!cfg.logging_dir.empty()) {
        auto session_log_dir = logging::session_log_directory(cfg.logging_dir);
        if(logging::file_logger(log_tag, session_log_dir, logging::options)) {
            LOG_INFO("Session log directory: {}", session_log_dir);
            run.log_dir = std::move(session_log_dir);
        }
    }

    WorkerPoolOptions pool_opts;
    // Stateful workers host open documents; a batch run has none.
    pool_opts.stateful_count = 0;
    pool_opts.stateless_count = cfg.stateless_worker_count;
    pool_opts.min_stateless = cfg.min_stateless_worker_count;
    pool_opts.max_stateless = cfg.max_stateless_worker_count;
    pool_opts.log_dir = run.log_dir;
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
#ifndef _WIN32
        aux.spawn(watch_termination(SIGHUP, stop_requested, interrupt));
#endif
        aux.spawn(checkpoints(run));
        aux.spawn(run.files.disk.end_turns(run.loop));
    }

    async::cancellation_token token() {
        return stop.token();
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

/// The subcommands of `clice index` that read the persisted index: its
/// stats and what it records of a symbol, a file and a unit.
std::vector<Operation> index_record_operations();

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
