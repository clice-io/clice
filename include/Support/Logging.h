#pragma once

#include "Format.h"

#include "spdlog/spdlog.h"

namespace clice::logging {

using Level = spdlog::level::level_enum;
using ColorMode = spdlog::color_mode;

struct Options {
    /// The logging level.
    Level level = Level::info;

    /// The logging color.
    ColorMode color = ColorMode::automatic;

    /// If enable, we will record the logs of console sink and replay it
    /// when create a new sink,
    bool replay_console = true;
};

extern Options options;

void stderr_logger(std::string_view name, const Options& options);

void file_loggger(std::string_view name, std::string_view dir, const Options& options);

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
    if(clice::logging::options.level <= clice::logging::Level::name) {                             \
        clice::logging::name(fmt __VA_OPT__(, ) __VA_ARGS__);                                      \
    }

#define LOG_TRACE(fmt, ...) LOG_MESSAGE(trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(info, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(warn, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(err, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...) clice::logging::critical(fmt __VA_OPT__(, ) __VA_ARGS__);

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
