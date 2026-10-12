// a.h and b.h include each other: compiled as partitions of core, they
// would import each other.
#pragma once

struct A {
    int v;
};

struct B;
#include "core/b.h"

inline int a_size() {
    return sizeof(B);
}
