#pragma once

/// What every compilation of clice force-includes: the macros, which no
/// module carries. The libraries' replays first, then clice's own.

#include "modules/macros.h"

#define LOG_MESSAGE(name, fmt, ...)                                                                \
    do {                                                                                           \
        if(clice::logging::options.level <= clice::logging::Level::name) {                         \
            clice::logging::log(clice::logging::Level::name,                                       \
                                std::source_location::current(),                                   \
                                std::format(fmt __VA_OPT__(, ) __VA_ARGS__));                      \
        }                                                                                          \
    } while(0)
#define LOG_TRACE(fmt, ...) LOG_MESSAGE(Trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(Debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(Info, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(Warn, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(Error, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...)                                                                        \
    clice::logging::fatal(std::source_location::current(),                                         \
                          std::format(fmt __VA_OPT__(, ) __VA_ARGS__))

/// Performance measurement line (see the taxonomy in logging.cppm): `topic`
/// must be a string literal, the message should be stable key=value pairs.
#define LOG_PERF(topic, fmt, ...)                                                                  \
    do {                                                                                           \
        if(clice::logging::perf_lines()) {                                                         \
            LOG_INFO("[perf:" topic "] " fmt __VA_OPT__(, ) __VA_ARGS__);                          \
        }                                                                                          \
    } while(0)

/// Soft assertion for internal invariants (see AnomalyId).
#define LOG_ANOMALY(id, fmt, ...)                                                                  \
    do {                                                                                           \
        if(clice::logging::should_report(clice::logging::AnomalyId::id)) {                         \
            clice::logging::report_anomaly(clice::logging::AnomalyId::id,                          \
                                           std::source_location::current(),                        \
                                           std::format(fmt __VA_OPT__(, ) __VA_ARGS__));           \
        }                                                                                          \
    } while(0)

/// User-facing guidance via window/showMessage (master) and the log file.
#define LOG_GUIDANCE(fmt, ...)                                                                     \
    do {                                                                                           \
        if(clice::logging::options.level <= clice::logging::Level::Warn) {                         \
            clice::logging::report_guidance(std::source_location::current(),                       \
                                            std::format(fmt __VA_OPT__(, ) __VA_ARGS__));          \
        }                                                                                          \
    } while(0)
