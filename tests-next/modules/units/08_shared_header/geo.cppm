// The header geo.cppm includes in its global module fragment, main.cpp
// includes too: the two readings of Point are one type.
module;
#include "point.h"
export module geo;

export using ::Point;

export int length(Point p) {
    return norm(p);
}
