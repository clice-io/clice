#pragma once

#include <algorithm>
#include <vector>

namespace clice::bench {

/// Percentile over a sample by the same nearest-rank formula the TS side
/// uses (tools/bench/perf.ts computeStats): index = floor(p * (n - 1)) on
/// the sorted values. Sorts in place; the sample must be non-empty.
inline double percentile(std::vector<double>& values, double p) {
    std::ranges::sort(values);
    auto index = static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
    return values[index];
}

}  // namespace clice::bench
