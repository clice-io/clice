#pragma once

#include "base.h"

namespace geo {

struct Point {
    int x;
    int y;
};

enum class Color : int { red, green };

struct Size;

int scale(int value, int factor = 2);
void move(Point& point, long dx);

struct Circle : Shape {
    double radius;

    double area() const override {
        return radius * radius * 3;
    }
};

inline int counter2 = counter + 1;

int Registry::count = 0;

namespace {
inline int local_bonus() {
    return local_seed() + 1;
}
}  // namespace

}  // namespace geo

namespace ext {
void report(double code);
}
