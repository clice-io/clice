module clice:support.timer;

import :support.prelude;

namespace clice {

/// Wall-clock timer for perf log lines (see LOG_PERF in support/logging.macros.h).
struct ScopedTimer {
    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    long long ms() const {
        return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start)
            .count();
    }

    /// Fractional milliseconds for sub-millisecond stages (feature compute,
    /// IPC hops); perf lines keep the `_ms` suffix convention.
    double ms_f() const {
        return chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - start)
                   .count() /
               1000.0;
    }
};

}  // namespace clice
