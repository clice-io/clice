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

    double circumference() const;

    double diameter() const;

    double radius;
};

class UnitCircle : public Circle {
public:
    UnitCircle() : Circle(1.0) {}

    double measure() const override;
};

double area(const Circle& circle);

}  // namespace shapes
