#include "shapes/circle.h"

int main() {
    return shapes::Circle(1.0).measure() > 3 ? 0 : 1;
}
