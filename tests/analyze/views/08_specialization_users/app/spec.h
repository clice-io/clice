#pragma once
#include "lib/traits.h"

template <>
struct Traits<int> {
    static int size() { return 4; }
};
