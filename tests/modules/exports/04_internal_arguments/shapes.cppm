export module shapes;

export template <class Shape>
double twice(const Shape& shape) {
    return 2 * shape.radius;
}

export auto make() {
    struct Made {
        double radius;
    };
    return Made{1.0};
}
