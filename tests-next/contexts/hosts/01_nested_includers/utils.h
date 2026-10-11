#pragma once

#include "inner.h"

inline int calc(Point p) {
    return distance(p, inner_origin());
}
