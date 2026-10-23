#pragma once

template <typename T>
int area(const T& shape) {
    return shape.width * shape.height;
}

inline int unit() {
    return 1;
}
