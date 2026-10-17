struct Shape {
    virtual double §(base)area() const = 0;
};

struct Circle : Shape {
    double §(override)area() const override {
        return 3.0;
    }
};

double through_base(const Shape& shape) {
    return shape.area();
}

double through_circle(const Circle& circle) {
    return circle.area();
}
