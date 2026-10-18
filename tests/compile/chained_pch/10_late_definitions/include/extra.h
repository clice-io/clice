#pragma once

#include "base.h"

struct IntTraits {
    static int get() {
        return 4;
    }
};

template <class T>
struct Pick;

template <>
struct Pick<int> {
    using type = IntTraits;
};

template <class T>
int Lazy<T>::run() const {
    return Pick<T>::type::get();
}

struct Probe {
    int later() const {
        return 1;
    }
};

inline int top_run() {
    return dependent(Probe{});
}
