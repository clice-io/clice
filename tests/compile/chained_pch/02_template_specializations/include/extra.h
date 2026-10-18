#pragma once

#include "base.h"

struct Widget {};

template <>
struct Traits<Widget> {
    static constexpr int value = 7;
};

template <class T>
struct Traits<T*> {
    static constexpr int value = 3;
};

template <>
inline int describe<Widget>(Widget) {
    return 5;
}

template <>
constexpr int weight<Widget> = 9;

template <>
inline int Holder<Widget>::size() const {
    return 4;
}

inline int top_use() {
    return twice(Widget{}) + Box<Widget>{}.get() + Holder<Widget>{}.size();
}
