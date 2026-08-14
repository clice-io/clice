#pragma once

#include <algorithm>
#include <cassert>
#include <vector>

namespace clice::bench {

/// Percentile over an ascending-sorted sample by the same nearest-rank
/// formula the TS side uses (tools/bench/perf.ts computeStats): index =
/// floor(p * (n - 1)). Takes the sample pre-sorted and non-empty: sorting
/// in place here would make call sites that read front()/back() in the
/// same argument list depend on argument evaluation order.
inline double percentile(const std::vector<double>& sorted, double p) {
    assert(!sorted.empty() && std::ranges::is_sorted(sorted));
    auto index = static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

}  // namespace clice::bench
