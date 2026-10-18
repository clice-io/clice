#pragma once

namespace geo {

struct Point;
enum class Color : int;

struct Size {
    int width;
    int height;
};

int scale(int value, int factor);
void move(Point& point, int dx);

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

inline int counter = 1;

struct Registry {
    static int count;
    static constexpr int limit = 8;
};

namespace {
inline int local_seed() {
    return 3;
}
}  // namespace

}  // namespace geo

namespace ext {
void report(int code);
}

namespace use {
using ext::report;
}

inline int base_area(const geo::Size& size) {
    return size.width * size.height;
}
