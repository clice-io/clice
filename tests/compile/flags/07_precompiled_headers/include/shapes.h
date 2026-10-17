#pragma once

struct Circle {
    double radius;
};

inline double area(const Circle& circle) {
    return 3.14159 * circle.radius * circle.radius;
}

[[deprecated("use area")]] inline double surface(const Circle& circle) {
    return area(circle);
}

inline double legacy(const Circle& circle) {
    return surface(circle);
}
