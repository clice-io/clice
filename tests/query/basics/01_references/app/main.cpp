#include "shapes/circle.h"

int main() {
    const shapes::Circle circle(2.0);
    const shapes::Shape& shape = circle;
    return static_cast<int>(shape.measure() + circle.circumference());
}
