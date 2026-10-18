#pragma once

template <class T>
struct Lazy {
    int run() const;
};

inline int base_run() {
    return Lazy<int>{}.run();
}

inline auto square = [](int x) { return x * x; };

template <class T>
int dependent(T value) {
    return value.later();
}

struct Probe;
