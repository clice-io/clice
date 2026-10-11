#include "include/geometry/shape.h"

int total(int width, int height) {
    return shape_area(width, height) + shape_perimeter(width, height);
}
