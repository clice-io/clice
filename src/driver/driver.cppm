module;

#include "modules/prelude.h"

#ifdef _WIN32
#include <io.h>
#include <print>
#endif

#include "support/logging.macros.h"

module clice:driver.driver;

import :config.config;
import :index.symbol_query;
import :index.writer_lock;
import :project.configuration;
import :sched.batch;
import :server.control_client;
import :server.query_commands;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :vfs.path;
import :worker.serialize;

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
    scan,
    analyze,
    modularize,
};

/// Runs a subcommand on the arguments after its name; returns the process
/// exit code.
using CommandHandler = kota::function<int(std::span<std::string>)>;

/// A subcommand as its file registers it: its line of `clice --help`, and
/// what makes its handler, given the path of this executable.
struct CommandEntry {
    std::string_view description;
    CommandHandler (*build)(const char* self_path) = nullptr;
};

inline auto& command_entries() {
    static std::array<CommandEntry, kota::meta::reflection<Command>::member_count> entries;
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
inline void add_commands(kota::deco::cli::SubCommander& root, const char* self_path) {
    for(auto command: kota::meta::reflection<Command>::member_values) {
        auto name = kota::meta::enum_name(command);
        auto& entry = command_entries()[std::to_underlying(command)];
        if(!entry.build) {
            logging::critical("The {} subcommand is not registered", name);
        }
        root.add({.name = name, .description = entry.description}, entry.build(self_path));
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
#ifdef _WIN32
    // A console takes UTF-16: std::print converts for it, where fwrite
    // would show UTF-8 in the console's code page.
    if(::_isatty(fd)) {
        std::print(stream, "{}", text);
        return;
    }
#endif
    if(std::fwrite(text.data(), 1, text.size(), stream) == text.size() &&
       std::fflush(stream) == 0) {
        return;
    }
    if(stream == stderr) {
        return;
    }
    int error = errno;
    if(error != EAGAIN && is_pipe_or_socket(fd)) {
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

/// The --log-level option every subcommand's options hold; a subcommand
/// that logs less unless asked gives it a default of its own.
struct LogLevelOption {
    DecoKV(help = "Log level", required = false)
    <LogLevel> log_level = LogLevel::Info;

    /// Make the level given the global log level.
    void apply() const {
        switch(*log_level) {
            case LogLevel::Trace: logging::options.level = logging::Level::trace; return;
            case LogLevel::Debug: logging::options.level = logging::Level::debug; return;
            case LogLevel::Info: logging::options.level = logging::Level::info; return;
            case LogLevel::Warn: logging::options.level = logging::Level::warn; return;
            case LogLevel::Error: logging::options.level = logging::Level::err; return;
            case LogLevel::Off: logging::options.level = logging::Level::off; return;
        }
        std::unreachable();
    }
};

/// The workspace a batch subcommand names: the --workspace argument
/// relative to the current directory, or the current directory when it is
/// empty, as spelled.
inline Spelling workspace_spelling(llvm::StringRef argument) {
    return Spelling(argument, Spelling::cwd());
}

/// Whether the workspace a batch subcommand names is a directory; one that
/// is not is reported, never created for the run's cache and logs.
inline bool workspace_exists(const Spelling& workspace) {
    if(vfs::is_directory(workspace)) {
        return true;
    }
    LOG_ERROR("{}: not a directory", workspace);
    return false;
}

/// The workspace root of a batch subcommand.
inline CanonicalPath workspace_root(llvm::StringRef argument) {
    return CanonicalPath(workspace_spelling(argument));
}

template <typename Parser>
void print_usage(Parser& cmd) {
    std::ostringstream ss;
    cmd.usage(ss);
    write_output(stdout, ss.str());
}

/// The error handler of a commander of subcommands: named without one, it
/// prints its usage and succeeds, as --help does; an unknown subcommand is
/// a usage error.
inline auto subcommand_error_handler(kota::deco::cli::SubCommander& commander) {
    return [&commander](const kota::deco::cli::SubCommandError& err) {
        if(err.type == kota::deco::cli::SubCommandError::Type::MissingSubCommand) {
            print_usage(commander);
            return 0;
        }
        driver::println(stderr, "{}", err.message);
        return kota::deco::cli::parse_error_exit_code;
    };
}

/// Run a commander of subcommands on `args`. The commander takes whatever
/// leads them for a subcommand's name; an option other than --help leading
/// them is reported as the unknown option it is.
inline int run_subcommands(kota::deco::cli::SubCommander& commander, std::span<std::string> args) {
    namespace text = kota::deco::cli::text;
    if(!args.empty() && args.front().starts_with("-") &&
       !llvm::is_contained(kota::deco::decl::help_names, args.front())) {
        driver::println(
            stderr,
            "{}",
            text::render_diagnostic(
                text::diagnostic_at(args, 0, 1, std::format("unknown option '{}'", args.front()))));
        return kota::deco::cli::parse_error_exit_code;
    }
    return commander(args);
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
    kota::ipc::protocol::optional<std::vector<query::SymbolEntry>> candidates;
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

/// The symbol a `--symbol` argument names, as a locator query.
inline std::expected<index::SymbolQuery, std::string>
    locator_of(const std::optional<std::string>& symbol) {
    if(!symbol) {
        return std::unexpected("name the symbol with --symbol");
    }
    return index::SymbolQuery::parse(*symbol);
}

/// Bring the index up to date with the disk before a --fresh answer:
/// through the serving writer when a server holds the lock, else by a
/// batch run of this process. Either runs a pass over the build under the hash
/// gate, so only units whose inputs changed are recompiled — and an
/// absent index gets built from nothing; `configuration` is one `config`
/// declares (check_requested_configuration). Returns the units that
/// failed to index, or why the refresh could not run.
inline std::expected<std::vector<std::string>, std::string> refresh(const Spelling& workspace,
                                                                    const Config& config,
                                                                    llvm::StringRef configuration,
                                                                    const char* self_path) {
    auto& cache_dir = config.project.cache_dir;
    auto writer = index::probe_writer(cache_dir);
    switch(writer.state) {
        case index::WriterProbe::State::Server: {
            auto result = control::request_index(writer.endpoint,
                                                 resolve_configuration(config, configuration));
            if(!result) {
                return std::unexpected(result.error());
            }
            return std::move(result->failed);
        }
        case index::WriterProbe::State::Held: {
            return std::unexpected(index::held_writer_message(writer, cache_dir));
        }
        case index::WriterProbe::State::Free: break;
    }
    auto report_progress = [](const BatchProgress& progress) {
        driver::println(stderr,
                        "indexing {}/{} units, {} failed",
                        progress.completed,
                        progress.total,
                        progress.failed);
    };
    auto result = run_batch_index({
        .root = workspace,
        .config = config,
        .configuration = configuration.str(),
        .self_path = self_path,
        .on_progress = report_progress,
    });
    if(!result.completed) {
        return std::unexpected(result.interrupted ? "indexing interrupted"
                                                  : "indexing failed; see the log");
    }
    if(result.unsaved) {
        return std::unexpected("part of the index could not be persisted; see the log");
    }
    return std::move(result.failed);
}

}  // namespace clice::driver
