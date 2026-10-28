// Each call and member access of a chain is a step of its own.
struct Size {
    int width;
};

struct Shape {
    Size bounds() const;
};

int measure(const Shape& shape) {
    return shape.§(method)bounds().§(field)width;
}
