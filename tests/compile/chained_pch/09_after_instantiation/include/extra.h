#pragma once

#include "base.h"

template <class T>
struct Traits<T*> {
    static constexpr int value = 3;
};

template <>
struct Traits<Unit> {
    static constexpr int value = 1;
};

template <>
inline int pick<Unit>(Unit) {
    return 1;
}

template <>
constexpr int weight<Unit> = 2;
