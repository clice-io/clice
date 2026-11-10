#pragma once

struct Shape {
    int sides;

    static int make(const Shape& shape);
};

int count(const Shape& shape);
