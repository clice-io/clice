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

    double radius;
};

double area(const Circle& circle);

}  // namespace shapes
