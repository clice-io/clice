#include "shape.h"

Shape::Shape() {}
Shape::~Shape() {}
double Shape::area() const { return 0; }

Circle::Circle(double r) : radius(r) {}
Circle::~Circle() {}
double Circle::area() const { return 3.14 * radius * radius; }
