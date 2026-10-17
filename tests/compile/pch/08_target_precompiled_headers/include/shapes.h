#pragma once

struct Circle {
    double radius;
};

inline double area(const Circle& circle) {
    return 3.14159 * circle.radius * circle.radius;
}
