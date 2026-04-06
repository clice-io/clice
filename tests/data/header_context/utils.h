// Non self-contained header: uses std::vector without including it.
// Depends on the including source file to provide the <vector> include.
#pragma once

inline std::vector<int> make_range(int n) {
    std::vector<int> result;
    for(int i = 0; i < n; ++i) {
        result.push_back(i);
    }
    return result;
}

inline int sum(const std::vector<int>& v) {
    int total = 0;
    for(int x: v) {
        total += x;
    }
    return total;
}
