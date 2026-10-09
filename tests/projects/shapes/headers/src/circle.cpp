#include "shapes/circle.h"
#include "shapes/detail/math.h"

namespace shapes {

double Circle::measure() const {
    return area(*this);
}

const char* Circle::name() const {
    return "circle";
}

double area(const Circle& circle) {
    return detail::scale(pi * detail::square(circle.radius));
}

}  // namespace shapes
