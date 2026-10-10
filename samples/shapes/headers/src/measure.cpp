#include "shapes/circle.h"

namespace shapes {

double Circle::circumference() const {
    return 2 * pi * radius;
}

double UnitCircle::measure() const {
    return pi;
}

}  // namespace shapes
