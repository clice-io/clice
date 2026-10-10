#include "shapes/circle.h"

int main() {
    shapes::Circle c(1.0);
    return shapes::area(c) > shapes_precision ? 0 : 1;
}
