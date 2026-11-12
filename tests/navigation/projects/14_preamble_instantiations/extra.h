#pragma once

struct Hidden {
    double radius;
};

template <typename T>
auto twice(T shape) {
    return 2 * shape.radius;
}

inline auto hidden_twice() {
    return twice(Hidden{2.0});
}
