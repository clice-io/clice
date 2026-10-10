#include "shapes/c_api.h"
#include "shapes/circle.h"
#include "shapes/polygon.h"
#include "shapes/registry.h"

int demo() {
    shapes::UnitCircle unit;
    return static_cast<int>(unit.measure()) + shapes::registry_count();
}

#include "shapes/detail/math.h"

double demo_square() {
    return shapes::detail::square(2.0);
}
