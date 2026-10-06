#pragma once

#define LOG_ANOMALY(id, fmt, ...)                                                                  \
    do {                                                                                           \
        if(clice::logging::anomaly_should_report(clice::logging::AnomalyId::id)) {                 \
            clice::logging::report_anomaly(clice::logging::AnomalyId::id,                          \
                                           std::format(fmt __VA_OPT__(, ) __VA_ARGS__));           \
        }                                                                                          \
    } while(0)
#define LOG_GUIDANCE(fmt, ...)                                                                     \
    do {                                                                                           \
        if(clice::logging::guidance_should_report()) {                                             \
            clice::logging::report_guidance(std::format(fmt __VA_OPT__(, ) __VA_ARGS__));          \
        }                                                                                          \
    } while(0)
