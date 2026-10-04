#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <print>
#endif

#include "support/logging.h"
#include "vfs/path.h"

#include "kota/deco/deco.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"

namespace clice::driver {

/// Each subcommand lives in its own translation unit under src/driver/ and
/// registers itself with the root commander through one of these builders.
/// `exit_code` is written by the matched handler when the command runs;
/// clice.cc owns the commander and the final process exit.

void add_serve(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_query(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_refactor(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_worker(kota::deco::cli::SubCommander& root, int& exit_code);
void add_index(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_lint(kota::deco::cli::SubCommander& root, int& exit_code, const char* self_path);
void add_format(kota::deco::cli::SubCommander& root, int& exit_code);
void add_inspect(kota::deco::cli::SubCommander& root, int& exit_code);
void add_analyze(kota::deco::cli::SubCommander& root, int& exit_code);
void add_modulize(kota::deco::cli::SubCommander& root, int& exit_code);

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
    if(error == EPIPE) {
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

/// Set the global log level from a user-supplied string; complains and
/// returns false on an unknown level.
inline bool apply_log_level(const std::string& level_str) {
    // from_str accepts mixed case, so the "off" sentinel comparison must
    // be case-insensitive too or `OFF` gets rejected as unknown.
    std::string lowered = level_str;
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    auto level = spdlog::level::from_str(lowered);
    if(level == spdlog::level::off && lowered != "off") {
        driver::println(stderr,
                        "unknown log level '{}', valid: trace, debug, info, warn, error, off",
                        level_str);
        return false;
    }
    logging::options.level = level;
    return true;
}

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

/// "s" when `count` warrants a plural noun, for user-facing summaries.
const inline char* plural_s(std::size_t count) {
    return count == 1 ? "" : "s";
}

}  // namespace clice::driver
