#pragma once

#include "numbers.h"

inline int four() {
    return twice(2);
}

template <class T>
T minus_one(T value) {
    return value - one();
}
