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
};

}  // namespace clice
