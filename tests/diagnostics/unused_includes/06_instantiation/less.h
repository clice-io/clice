#pragma once
#include "point.h"
inline bool operator<(Point a, Point b) {
    return a.x < b.x;
}
