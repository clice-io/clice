#pragma once

// Non self-contained: uses std::vector from the include chain.
inline int inner_sum(const std::vector<int>& v) {
    int total = 0;
    for(int x: v) {
        total += x;
    }
    return total;
}
