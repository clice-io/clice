export module shapes:circle;

export namespace shapes {

struct Circle {
    double radius;
};

double area(const Circle& circle);

}  // namespace shapes
