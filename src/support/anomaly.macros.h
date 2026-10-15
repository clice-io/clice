#pragma once

/// Soft assertion for internal invariants (see AnomalyId). The gate runs
/// before the format arguments are evaluated, so suppressed reports cost
/// nothing — arguments must not carry needed side effects.
#define LOG_ANOMALY(id, fmt, ...)                                                                  \
    do {                                                                                           \
        if(clice::logging::anomaly_should_report(clice::logging::AnomalyId::id)) {                 \
            clice::logging::report_anomaly(clice::logging::AnomalyId::id,                          \
                                           std::format(fmt __VA_OPT__(, ) __VA_ARGS__));           \
        }                                                                                          \
    } while(0)

/// User-facing guidance via window/logMessage (master) and the log file.
/// Same lazy-evaluation contract as LOG_ANOMALY.
#define LOG_GUIDANCE(fmt, ...)                                                                     \
    do {                                                                                           \
        if(clice::logging::guidance_should_report()) {                                             \
            clice::logging::report_guidance(std::format(fmt __VA_OPT__(, ) __VA_ARGS__));          \
        }                                                                                          \
    } while(0)
