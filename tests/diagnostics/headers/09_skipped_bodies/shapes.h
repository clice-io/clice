#pragma once

template <typename T>
int area(const T& shape) {
    return shape.width * shape.height;
}

inline int unit() {
    return 1;
}

template <typename F>
int apply(F f) {
    return f(1);
}
