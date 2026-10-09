#include "../include/geometry/shape.h"

int shape_area(int width, int height) {
    return width * height;
}

int shape_perimeter(int width, int height) {
    return 2 * (width + height);
}
