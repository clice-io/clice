#include "shapes/c_api.h"

import shapes;

int demo() {
    shapes::UnitCircle unit;
    return static_cast<int>(unit.measure()) + shapes::registry_count();
}

#include "shapes/config.h"

int demo_precision() {
    return shapes_precision;
}

int demo_capacity() {
    shapes::Circle circle = shapes::make_circle(1.0);
    const shapes::Shape& shape = circle;
    shapes::registry_add(shape);
    return shapes::registry_capacity();
}
