#include "../include/geometry/shape.h"

int check_shape() {
    return shape_area(2, 3) == 6 ? 0 : 1;
}
