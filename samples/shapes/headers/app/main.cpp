#include "shapes/c_api.h"
#include "shapes/circle.h"
#include "shapes/polygon.h"
#include "shapes/registry.h"

int main() {
    shapes::Circle c(2.0);
    shapes::Polygon<3> triangle(1.0);
    double total = shapes::area(c) + triangle.measure() + shapes_circle_area(1.0);
    return static_cast<int>(total) + shapes::registry_count();
}
