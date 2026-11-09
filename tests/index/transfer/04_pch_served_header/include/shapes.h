#pragma once

struct Circle {
    double radius;
};

template <class Shape>
double twice(const Shape& shape) {
    return 2 * shape.radius;
}
