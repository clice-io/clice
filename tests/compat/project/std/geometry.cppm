export module compat.shapes:geometry;

import std;

export namespace compat {

struct Circle {
    double radius = 0;

    double area() const {
        return std::numbers::pi * radius * radius;
    }
};

}  // namespace compat
