struct Shape {
    virtual double §(pure)area() const = 0;
};

struct Circle : Shape {
    double area() const override {
        return 3.0;
    }
};

struct Square : Shape {
    double area() const override {
        return 4.0;
    }
};

double measure(const Shape& shape) {
    return shape.§(call)area();
}
