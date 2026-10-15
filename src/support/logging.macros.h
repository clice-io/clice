#pragma once

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

/// Performance measurement line (see the taxonomy in logging.cppm): `topic`
/// must be a string literal, the message should be stable key=value pairs.
#define LOG_PERF(topic, fmt, ...) LOG_INFO("[perf:" topic "] " fmt __VA_OPT__(, ) __VA_ARGS__)
