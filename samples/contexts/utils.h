// Non self-contained header: uses Point from types.h without including it.
// Depends on the including source file to provide the types.h include.
#pragma once

#include "inner.h"

inline int calc(Point p) {
    return distance(p, inner_origin());
}
