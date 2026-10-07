module;

#include "modules/prelude.h"

#ifdef _WIN32
#include <io.h>
#include <print>
#endif

#include "support/logging.macros.h"

module clice:driver.driver;

import :support.logging;
import :vfs.path;

namespace clice::driver {

/// Each subcommand lives in its own translation unit under src/driver/ and
/// registers itself with the root commander through one of these builders;
/// its handler returns the process exit code.

void add_serve(kota::deco::cli::SubCommander& root, const char* self_path);
void add_query(kota::deco::cli::SubCommander& root, const char* self_path);
void add_refactor(kota::deco::cli::SubCommander& root, const char* self_path);
void add_worker(kota::deco::cli::SubCommander& root);
void add_index(kota::deco::cli::SubCommander& root, const char* self_path);
void add_lint(kota::deco::cli::SubCommander& root, const char* self_path);
void add_format(kota::deco::cli::SubCommander& root);
void add_inspect(kota::deco::cli::SubCommander& root);
void add_analyze(kota::deco::cli::SubCommander& root);
void add_modularize(kota::deco::cli::SubCommander& root);

/// Write command output. stdout carries the command's answer: a reader
/// that went away (`clice … | head`) ends the process by SIGPIPE, as it
/// ends any command-line tool — clice ignores SIGPIPE, or a dead worker's
/// pipe would take the master down — and any other failure (a full disk
/// under a redirect) is reported and exits 1. stderr carries progress and
/// complaints, which, like log lines, are dropped when they cannot be
/// written: a progress line must not end a run before it saves.
inline void write_output(std::FILE* stream, std::string_view text) {
#ifdef _WIN32
    // A console takes UTF-16: std::print converts for it, where fwrite
    // would show UTF-8 in the console's code page.
    if(::_isatty(::_fileno(stream))) {
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
#ifndef _WIN32
    // A socket reader (Node's stdio pipes are socketpairs) that went away
    // with output still unread fails the write with ECONNRESET instead, or
    // on macOS at times with EPROTOTYPE, which libuv maps the same way.
    if(error == EPIPE || error == ECONNRESET || error == EPROTOTYPE) {
        std::signal(SIGPIPE, SIG_DFL);
        std::raise(SIGPIPE);
    }
#endif
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
    DecoKV(style = kota::deco::decl::KVStyle::JoinedOrSeparate,
           help = "Log level",
           required = false)
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

/// The workspace root of a batch subcommand.
inline CanonicalPath workspace_root(llvm::StringRef argument) {
    return CanonicalPath(workspace_spelling(argument));
}

template <typename Command>
void print_usage(Command& cmd) {
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

/// "s" when `count` warrants a plural noun, for user-facing summaries.
const inline char* plural_s(std::size_t count) {
    return count == 1 ? "" : "s";
}

}  // namespace clice::driver
