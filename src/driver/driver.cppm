module;

#include "support/prelude.h"

#include <errno.h>

#include "support/logging.macros.h"

module clice:driver.driver;

import :config.config;
import :index.writer_lock;
import :project.configuration;
import :sched.batch;
import :server.control_client;
import :server.query_commands;
import :support.json;
import :support.logging;
import :support.process;
import :vfs.file_system;
import :vfs.path;

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
    if(write_console(fd, text)) {
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

/// The workspace a subcommand works in, relative to the current directory,
/// and its build configuration. The files it names are relative to the
/// workspace.
struct WorkspaceOptions {
    DecoKV(help = "Workspace root directory (default: the current directory)", required = false)
    <std::string> workspace;

    ConfigurationOption configuration;

    /// The workspace as spelled: the current directory when none is given.
    Spelling root() const {
        return Spelling(workspace.value_or(""), Spelling::cwd());
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
    DecoInput(meta_var = "<PATH>",
              help = "The file, relative to the workspace or absolute",
              required = false)
    <std::vector<std::string>> inputs;
};

/// The symbol an operation is about.
struct SymbolTarget {
    DecoInput(meta_var = "<LOCATOR>",
              help = "The symbol, as a locator: a name, an answer's symbol, or file:line[:column]",
              required = false)
    <std::vector<std::string>> inputs;
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

/// Whether the workspace a batch subcommand names is a directory; one that
/// is not is reported, never created for the run's cache and logs.
inline bool workspace_exists(const Spelling& workspace) {
    if(vfs::is_directory(workspace)) {
        return true;
    }
    LOG_ERROR("{}: not a directory", workspace);
    return false;
}

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
    protocol::optional<std::vector<query::SymbolEntry>> candidates;
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
    return kota::deco::cli::parse_error_exit_code;
}

/// The positional arguments given, named `names` as the usage spells
/// them, of which the trailing `optional` may be left out.
inline std::expected<std::vector<std::string>, std::string>
    positional(const std::optional<std::vector<std::string>>& given,
               llvm::ArrayRef<std::string_view> names,
               std::size_t optional = 0) {
    auto args = given.value_or(std::vector<std::string>{});
    if(args.size() + optional < names.size()) {
        return std::unexpected(std::format("missing {}", names[args.size()]));
    }
    if(args.size() > names.size()) {
        return std::unexpected(std::format("unexpected argument '{}'", args[names.size()]));
    }
    return args;
}

/// A subcommand, or one operation of it, over the options `Options`, whose
/// LogLevelOption is `log`: it applies the level, logs to stderr as
/// `logger` and returns what `run` does with the options.
template <typename Options, typename Run>
CommandHandler operation(std::string_view usage, std::string_view logger, Errors errors, Run run) {
    auto command = kota::deco::cli::command<Options>(usage);
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
inline auto subcommand_error_handler(kota::deco::cli::SubCommander& commander,
                                     Errors errors = Errors::Text) {
    return [&commander, errors](const kota::deco::cli::SubCommandError& err) {
        if(err.type == kota::deco::cli::SubCommandError::Type::MissingSubCommand) {
            print_usage(commander);
            return 0;
        }
        return usage_error(errors, err.message);
    };
}

/// Run a commander of subcommands on `args`. The commander takes whatever
/// leads them for a subcommand's name; an option other than --help leading
/// them is reported as the unknown option it is.
inline int run_subcommands(kota::deco::cli::SubCommander& commander,
                           std::span<std::string> args,
                           Errors errors = Errors::Text) {
    namespace text = kota::deco::cli::text;
    if(!args.empty() && args.front().starts_with("-") &&
       !ranges::contains(kota::deco::decl::help_names, args.front())) {
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
    auto commander = std::make_shared<kota::deco::cli::SubCommander>("", main ? "" : overview);
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
               return ranges::contains(kota::deco::decl::help_names, arg);
           })) {
            driver::println();
            print_usage(*commander);
        }
        return code;
    };
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
    if(result.damaged) {
        return std::unexpected(
            "the index database is damaged (see the log); it is discarded, and the next `clice "
            "index` rebuilds the index");
    }
    return std::move(result.failed);
}

}  // namespace clice::driver
