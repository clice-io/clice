#include "include/geometry.h"

// Multiplies; no overflow check.
int area(int width, int height) {
    return width * height;
}

int surface(int width, int height) {
    return area(width, height);
}

void Shape::draw() {}

void Square::draw() {}
