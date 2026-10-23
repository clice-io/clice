#pragma once

#include "numbers.h"

inline auto four() {
    return twice(2);
}

template <class T>
T minus_one(T value) {
    return value - one();
}
