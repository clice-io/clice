#include "shapes/circle.h"

namespace shapes {

double Circle::measure() const {
    return 3.14159 * radius * radius;
}

double Circle::circumference() const {
    return 2 * 3.14159 * radius;
}

}  // namespace shapes
