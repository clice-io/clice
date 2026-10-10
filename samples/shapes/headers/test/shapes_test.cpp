#include "shapes/shapes.h"
#include "shapes/shape.h"

int main() {
    shapes::Circle c(1.0);
    return shapes::area(c) > shapes_precision ? 0 : 1;
}
