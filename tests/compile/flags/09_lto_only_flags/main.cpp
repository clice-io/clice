struct Shape {
    virtual int area();
};

int measure(Shape& shape) {
    return shape.area();
}
