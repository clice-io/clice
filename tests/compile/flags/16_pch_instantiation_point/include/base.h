#pragma once

inline int helper(...) {
    return 0;
}

template <class T>
int apply(T value) {
    return helper(value);
}

struct Early {};

inline int base_use() {
    return apply(Early{});
}
