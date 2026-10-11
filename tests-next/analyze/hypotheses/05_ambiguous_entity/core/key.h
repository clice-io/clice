#pragma once
#include "third/lib.h"

struct Key {
    int k;
};

template <>
struct Traits<Key> {
    static constexpr int value = 2;
};
