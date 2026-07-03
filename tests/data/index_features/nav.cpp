#include "nav.h"

int area(const Shape& s) {
    return s.width * s.height;
}

Shape global_shape;

int shape_area() {
    return area(global_shape);
}
