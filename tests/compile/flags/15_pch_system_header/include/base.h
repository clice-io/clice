#pragma once

/// A point on the grid.
struct Point {
    int x;
    int y;

    int norm() const {
        return x + y;
    }
};

template <class T>
int measure(const T& value) {
    return value.norm();
}
