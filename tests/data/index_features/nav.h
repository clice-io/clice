#pragma once

struct Shape;

int area(const Shape& s);

struct Shape {
    int width;
    int height;
};
