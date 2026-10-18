export module shapes;

export template <class Shape>
double twice(const Shape& shape) {
    return 2 * shape.radius;
}
