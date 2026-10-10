module;

#include "legacy/units.h"

export module shapes:circle;

import :shape;

export namespace shapes {

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

Circle make_circle(double radius);

}  // namespace shapes
