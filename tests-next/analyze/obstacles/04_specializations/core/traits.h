// Specializes a third-party template for Str, which core/str.h provides:
// every instantiation uses it without naming it.
#pragma once
#include "core/str.h"
#include "third/lib.h"

template <>
struct Traits<Str> {
    static constexpr int value = 1;
};
