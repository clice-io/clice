#pragma once

#define LOG_MESSAGE(name, fmt, ...)                                                                \
    do {                                                                                           \
        if(clice::logging::options.level <= clice::logging::Level::name) {                         \
            clice::logging::log(clice::logging::Level::name,                                       \
                                std::source_location::current(),                                   \
                                fmt __VA_OPT__(, ) __VA_ARGS__);                                   \
        }                                                                                          \
    } while(0)
#define LOG_TRACE(fmt, ...) LOG_MESSAGE(trace, fmt, __VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_MESSAGE(debug, fmt, __VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_MESSAGE(info, fmt, __VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_MESSAGE(warn, fmt, __VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_MESSAGE(err, fmt, __VA_ARGS__)
#define LOG_FATAL(fmt, ...)                                                                        \
    clice::logging::fatal(std::source_location::current(), fmt __VA_OPT__(, ) __VA_ARGS__)

/// Performance measurement line (see the taxonomy in logging.cppm): `topic`
/// must be a string literal, the message should be stable key=value pairs.
#define LOG_PERF(topic, fmt, ...) LOG_INFO("[perf:" topic "] " fmt __VA_OPT__(, ) __VA_ARGS__)

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
        if(clice::logging::options.level <= clice::logging::Level::warn) {                         \
            clice::logging::report_guidance(std::source_location::current(),                       \
                                            std::format(fmt __VA_OPT__(, ) __VA_ARGS__));          \
        }                                                                                          \
    } while(0)
