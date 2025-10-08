#pragma once

#include "Format.h"
#include "spdlog/spdlog.h"

namespace clice::logging {

using Level = spdlog::level::level_enum;

struct Options {
    Level level = Level::info;
    bool color = false;
};

extern Options options;

void create_stderr_logger(std::string_view name, const Options& options);

void create_file_loggger(std::string_view name, std::string_view dir, const Options& options);

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
void fatal [[noreturn]] (logging_format<Args...> fmt, Args&&... args) {
    logging::log(spdlog::level::err, fmt.location, fmt.str, std::forward<Args>(args)...);
    spdlog::shutdown();
    std::exit(1);
}

}  // namespace clice::logging
