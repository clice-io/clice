#pragma once

const double scale = 2.0;

static double helper(double value) {
    return value * scale;
}

namespace {
struct Hidden {
    double radius;
};
}  // namespace

template <class Shape>
double twice(const Shape& shape) {
    return 2 * shape.radius;
}

inline auto make() {
    struct Made {
        double value;
    };
    return Made{1.0};
}
