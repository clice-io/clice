export module circle;

export struct Circle {
    double radius;
};

export double §(declaration)area(const Circle& circle) {
    return 3.14159 * circle.radius * circle.radius;
}
