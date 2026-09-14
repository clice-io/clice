#pragma once

namespace shapes {

struct Rect {
    int width, height;
    ~Rect();
};

inline Rect::~Rect() {}

inline int width(const Rect& r) { return r.width; }

template <typename T>
T twice(T value) {
    return value + value;
}

}  // namespace shapes
