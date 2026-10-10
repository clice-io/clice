#pragma once

#include "shapes/shape.h"

namespace shapes {

double edge(double length);

template <int N>
class Polygon : public Shape {
public:
    explicit Polygon(double length) : length(length) {}

    double measure() const override {
        return perimeter();
    }

    const char* name() const override {
        return "polygon";
    }

    double perimeter() const {
        return N * side();
    }

    double side() const {
        return edge(length);
    }

    double length;
};

template <>
class Polygon<3> : public Shape {
public:
    explicit Polygon(double length) : length(length) {}

    double measure() const override;

    const char* name() const override;

    double length;
};

}  // namespace shapes
