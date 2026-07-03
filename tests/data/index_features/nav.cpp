#include "nav.h"

int area(const Shape& s) {
    return s.width * s.height;
}

Shape global_shape;

int shape_area() {
    return area(global_shape);
}

void GLRenderer::render() {}

void DebugGLRenderer::render() {}

Shape make_unit_shape() {
    return {1, 1};
}
