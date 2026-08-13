#pragma once

#include <chrono>

namespace clice {

/// Wall-clock timer for perf log lines (see LOG_PERF in support/logging.h).
struct ScopedTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    long long ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }

    /// Fractional milliseconds for sub-millisecond stages (feature compute,
    /// IPC hops); perf lines keep the `_ms` suffix convention.
    double ms_f() const {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count() /
               1000.0;
    }
};

}  // namespace clice
