module;

#include "shapes/config.h"

export module shapes:shape;

export namespace shapes {

class SHAPES_API Shape {
public:
    virtual ~Shape() = default;

    virtual double measure() const = 0;

    virtual const char* name() const = 0;
};

}  // namespace shapes
