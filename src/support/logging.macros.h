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
