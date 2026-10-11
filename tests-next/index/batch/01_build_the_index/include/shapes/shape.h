#pragma once

namespace shapes {

class Shape {
public:
    virtual ~Shape() = default;

    virtual double measure() const = 0;
};

}  // namespace shapes
