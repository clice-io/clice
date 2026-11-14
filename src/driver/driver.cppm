module;

#include <errno.h>

module clice:driver.driver;

import :feature.query;
import :project.discovery;
import :support.json;
import :support.logging;
import :support.prelude;
import :vfs.path;

namespace clice::driver {

/// The subcommands of `clice`, each run for the process's exit code. A
/// subcommand's file adds it (cli::add_command) from the initializer of an
/// ordinary variable at namespace scope: the linker's garbage collection
/// (--gc-sections, /OPT:REF, LTO) may drop a variable no one reads, with
/// its initializer, when it is a COMDAT of its own, as an inline variable
/// or a template's static member is, but never an ordinary one. Nothing
/// names a subcommand's file either, so the driver library is linked whole
/// (alwayslink).
struct Subcommands {
    using signature = int();
};

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

/// --configuration: the build configuration a subcommand works in.
struct ConfigurationOption {
    /// The tag given, empty for none.
    KANNOTATE(&configuration,
              description =
                  "Build configuration, one of the tags declared on rules (default: the selected "
                  "one, else default_configuration)")
    std::string configuration;
};

/// The project a subcommand works on, and its build configuration.
struct ProjectOptions {
    KANNOTATE(&workspace,
              description =
                  "Project root directory (default: the nearest directory at or above the current "
                  "one holding a clice.toml or a compile_commands.json)")
    std::optional<std::string> workspace;

    ConfigurationOption build;

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
    KANNOTATE(&jobs,
              cli::names = {"-j", "--jobs"},
              description =
                  "How many units or files to work on at once; 0 for "
                  "project.stateless_worker_count, or for index and format the CPU count")
    std::uint32_t jobs = 0;
};

/// --json, of the batch subcommands that report a run.
struct JsonReportOption {
    KANNOTATE(&json, description = "Print the run's report as JSON on stdout instead of text")
    bool json = false;
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
    KANNOTATE(&path,
              cli::positional = true,
              cli::meta_var = "<PATH>",
              description = "The file, relative to the current directory or absolute")
    std::string path;
};

/// The symbol an operation is about.
struct SymbolTarget {
    KANNOTATE(
        &locator,
        cli::positional = true,
        cli::meta_var = "<LOCATOR>",
        description = "The symbol, as a locator: a name, an answer's symbol, or file:line[:column]")
    std::string locator;
};

/// --fresh, of the subcommands that read the index.
struct FreshOption {
    KANNOTATE(&fresh,
              description =
                  "Reindex the files whose content changed since they were indexed "
                  "first, through the running server or a batch run of this command")
    bool fresh = false;
};

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

/// Report a command line that does not parse; returns its exit code. A
/// subcommand answering in JSON answers it with the Failure on stdout.
inline int usage_error(const cli::Error& error) {
    constexpr std::array<std::string_view, 4> answering_json = {
        "query",
        "refactor",
        "analyze",
        "modularize",
    };
    if(error.subcommands.empty() || !ranges::contains(answering_json, error.subcommands.front())) {
        cli::report(error);
        return 2;
    }
    std::string message;
    error.summary(message);
    print_json(Failure{.error = std::move(message)});
    return 2;
}

template <typename T>
int dispatch(T& options);

template <typename Tag>
int run_operation(cli::Commands<Tag>& operation) {
    return operation();
}

template <typename... Operations>
int run_operation(std::variant<Operations...>& operation) {
    return std::visit([](auto& chosen) { return dispatch(chosen); }, operation);
}

/// Run what `options` ask for: the operation they name (`operation`, a
/// std::variant of them or a cli::Commands), which one without a job of its
/// own requires; else its own job, the overload of `run` its file declares,
/// its log on stderr.
template <typename T>
int dispatch(T& options) {
    if constexpr(!requires { run(options); }) {
        return run_operation(options.operation);
    } else {
        if constexpr(requires { options.operation; }) {
            if(options.operation) {
                return run_operation(*options.operation);
            }
        }
        logging::stderr_logger("clice");
        return run(options);
    }
}

/// Why a subcommand cannot work on its project, and the exit code it ends
/// with: 2, the work not done (docs/en/cli/overview.md).
struct ProjectError {
    std::string message;
    int exit_code = 2;
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

}  // namespace clice::driver
