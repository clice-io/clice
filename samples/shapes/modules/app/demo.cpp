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
