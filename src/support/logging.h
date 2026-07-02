#pragma once

#include <concepts>
#include <cstdlib>
#include <format>
#include <source_location>
#include <string_view>
#include <type_traits>

#include "support/format.h"

#include "spdlog/spdlog.h"

/// # Logging taxonomy
///
/// clice reports through several channels; pick by audience and purpose:
///
/// ## File/console log — LOG_TRACE .. LOG_FATAL
/// The developer-facing record of what the server did.
/// - trace: extremely verbose, per-token / per-payload detail.
/// - debug: control-flow detail for chasing a specific bug (per-lookup
///   cache decisions, scheduler state changes, generation checks).
/// - info: one line per meaningful event — startup steps, per-file
///   compile/index results, command decision logs. This is the default
///   level; an info-level session log must be enough to reconstruct what
///   the server did and why.
/// - warn: something went wrong but was handled or is expected in operation
///   (user-code compile errors, worker-down windows, cancelled requests).
/// - err: an operation failed and its result is lost or wrong; the server
///   keeps running.
/// - critical (LOG_FATAL): unrecoverable; flushes the log and aborts.
///
/// ## Perf lines — LOG_PERF(topic, ...)
/// Greppable measurements for profiling on real codebases, rendered as
/// "[perf:<topic>] key=value ..." at info level, e.g.
/// `grep 'perf:index' master.log`. Topics in use: "index" (per-file and
/// phase timings), "cache" (hit/miss with reason), "startup" (phase
/// durations). Use stable key=value pairs and `_ms` suffixes for durations —
/// scripts aggregate these lines.
///
/// ## Anomalies — LOG_ANOMALY(id, ...) (support/anomaly.h)
/// Soft assertions for states that are unreachable unless clice itself is
/// buggy. Debug builds abort (see CLICE_ANOMALY_NO_TRAP); Release logs
/// "[anomaly:<id>]" and pushes window/logMessage. Never use for conditions
/// reachable through user input or normal operation.
///
/// ## Guidance — LOG_GUIDANCE(...) (support/anomaly.h)
/// User-actionable situations that are not clice bugs (missing CDB, invalid
/// configuration), pushed to the client via window/logMessage as warnings.
/// File-specific guidance is additionally published as diagnostics
/// (inferred-compile-command, clice.toml issues).
///
/// ## Process ownership
/// The master logs to <logging_dir>/<session>/master.log and mirrors to
/// stderr, which editors show in their output panel. Workers log ONLY to
/// their own <session>/<worker>.log (mirror_stderr = false): a worker's
/// stderr is reserved for unexpected third-party output — assertion
/// failures, sanitizer reports — which the pool relays line-by-line into the
/// master log. Crash backtraces are appended to the owning process's log
/// file by install_crash_handler (error signals including SIGABRT, so
/// LOG_FATAL and Debug anomaly traps are covered); a worker's backtrace goes
/// ONLY to its own log file, while the master's is also printed to stderr.
/// Before file_logger runs, backtraces reach stderr only (master) or are
/// lost (workers without a log dir).
namespace clice::logging {

using Level = spdlog::level::level_enum;
using ColorMode = spdlog::color_mode;

struct Options {
    Level level = Level::info;
    ColorMode color = ColorMode::automatic;
    bool replay_console = true;
};

extern Options options;

void stderr_logger(std::string_view name, const Options& options);

/// Log to <dir>/<name>.log, replaying lines buffered by stderr_logger.
/// With mirror_stderr, every line is also written to stderr — the master
/// uses this so editors can show its log; workers must pass false (their
/// stderr is reserved for crash output, relayed by the pool).
void file_logger(std::string_view name,
                 std::string_view dir,
                 const Options& options,
                 bool mirror_stderr = true);

/// Install a signal handler that writes crash stacktraces to the given log
/// file. With stderr_trace, LLVM's stderr stacktrace output is enabled too —
/// the master wants it (editors capture its stderr), workers must not (their
/// stderr pipe would relay the trace into the master log; a worker's crash
/// trace belongs in its own log file only).
/// Must be called after file_logger so the log file path is known.
void install_crash_handler(std::string_view log_path, bool stderr_trace = true);

template <typename... Args>
struct logging_rformat {
    template <std::convertible_to<std::string_view> StrLike>
    consteval logging_rformat(const StrLike& str,
                              std::source_location location = std::source_location::current()) :
        str(str), location(location) {}

    std::format_string<Args...> str;
    std::source_location location;
};

template <typename... Args>
using logging_format = logging_rformat<std::type_identity_t<Args>...>;

template <typename... Args>
void log(spdlog::level::level_enum level,
         std::source_location location,
         std::format_string<Args...> fmt,
         Args&&... args) {
    spdlog::source_loc loc{
        location.file_name(),
        static_cast<int>(location.line()),
        location.function_name(),
    };
    using spdlog_fmt = spdlog::format_string_t<Args...>;
    if constexpr(std::same_as<spdlog_fmt, std::string_view>) {
        spdlog::log(loc, level, fmt.get(), std::forward<Args>(args)...);
    } else {
        spdlog::log(loc, level, fmt, std::forward<Args>(args)...);
    }
}

template <typename... Args>
void trace(logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::trace, fmt.location, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void debug(logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::debug, fmt.location, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void info(logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::info, fmt.location, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void warn(logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::warn, fmt.location, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void err(logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::err, fmt.location, fmt.str, std::forward<Args>(args)...);
}

template <typename... Args>
void critical [[noreturn]] (logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::critical, fmt.location, fmt.str, std::forward<Args>(args)...);
    spdlog::shutdown();
    std::abort();
}

}  // namespace clice::logging

#define LOG_MESSAGE(name, fmt, ...)                                                                \
    do {                                                                                           \
        if(clice::logging::options.level <= clice::logging::Level::name) {                         \
            clice::logging::name(fmt __VA_OPT__(, ) __VA_ARGS__);                                  \
        }                                                                                          \
    } while(0)

#define LOG_TRACE(fmt, ...) LOG_MESSAGE(trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(info, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(warn, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(err, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...) clice::logging::critical(fmt __VA_OPT__(, ) __VA_ARGS__);

/// Performance measurement line (see the taxonomy above): `topic` must be a
/// string literal, the message should be stable key=value pairs.
#define LOG_PERF(topic, fmt, ...) LOG_INFO("[perf:" topic "] " fmt __VA_OPT__(, ) __VA_ARGS__)

#define LOG_MESSAGE_RET(ret, name, fmt, ...)                                                       \
    do {                                                                                           \
        LOG_MESSAGE(name, fmt, __VA_ARGS__);                                                       \
        return ret;                                                                                \
    } while(0);

#define LOG_TRACE_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, trace, fmt, __VA_ARGS__)
#define LOG_DEBUG_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, debug, fmt, __VA_ARGS__)
#define LOG_INFO_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, info, fmt, __VA_ARGS__)
#define LOG_WARN_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, warn, fmt, __VA_ARGS__)
#define LOG_ERROR_RET(ret, fmt, ...) LOG_MESSAGE_RET(ret, err, fmt, __VA_ARGS__)
