#pragma once

#include "legacy/units.h"
#include "shapes/shape.h"

namespace shapes {

inline constexpr double pi = SHAPES_PI;

class Circle : public Shape {
public:
    explicit Circle(double radius) : radius(radius) {}

    double measure() const override;

    const char* name() const override;

    double radius;
};

double area(const Circle& circle);

}  // namespace shapes
