#include "shapes/circle.h"

namespace shapes {

double area(const Circle& circle) {
    return 3.14159 * circle.radius * circle.radius;
}

}  // namespace shapes
