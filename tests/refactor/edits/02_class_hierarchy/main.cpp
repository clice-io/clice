#include "shape.h"

template <typename T>
double measure(const T& shape) { return shape.area(); }

double total(const Shape& s) { return s.area() + measure(Circle(1.0)); }

int main() {
    Circle c{2.0};
    Shape* p = &c;
    return static_cast<int>(p->area() + total(c));
}
