#ifndef SHAPES_H
#define SHAPES_H

#include "point.h"

struct §(circle)Circle {
    Point center;
    double radius;
};

inline double area(const Circle& circle) {
    return 3.14159 * circle.§(radius)radius * circle.radius;
}

#endif
