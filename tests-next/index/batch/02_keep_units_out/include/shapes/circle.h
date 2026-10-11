#pragma once

#include "shapes/shape.h"

namespace shapes {

class Circle : public Shape {
public:
    explicit Circle(double radius) : radius(radius) {}

    double measure() const override;

    double circumference() const;

    double radius;
};

}  // namespace shapes
