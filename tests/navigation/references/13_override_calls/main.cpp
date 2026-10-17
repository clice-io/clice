struct Shape {
    virtual double §(base)area() const {
        return 0.0;
    }

    virtual double scale(int factor) const {
        return factor;
    }

    virtual double scale(double factor) const {
        return factor;
    }
};

struct Circle : Shape {
    double §(override)area() const override {
        return Shape::area() + 3.0;
    }

    double §(scale)scale(int factor) const override {
        return factor * 2.0;
    }
};

double through_base(const Shape& shape) {
    return shape.area() + shape.scale(2);
}

double through_circle(const Circle& circle) {
    return circle.area();
}

template <typename T>
double base_scale(const Circle& circle, T factor) {
    return circle.Shape::scale(factor);
}
