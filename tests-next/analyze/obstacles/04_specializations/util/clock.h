// Specializes core's Hash: legal, but reachable only where util is
// imported.
#pragma once
#include "core/hash.h"

struct Clock {
    int ticks;
};

template <>
struct Hash<Clock> {
    static constexpr int seed = 7;
};
