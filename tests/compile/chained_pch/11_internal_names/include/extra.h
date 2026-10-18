#pragma once

#include "base.h"

namespace {
struct Ring {
    double radius;
};
}  // namespace

static double ring_twice() {
    return twice(Ring{helper(scale)});
}

inline double hidden_twice() {
    return twice(Hidden{1.0});
}
