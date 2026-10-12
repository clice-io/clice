#pragma once
#include "core/str.h"
#include "third/lib.h"

template <>
struct Traits<Str> {
    static constexpr int value = 1;
};
