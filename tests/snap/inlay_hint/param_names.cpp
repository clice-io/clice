/// # Parameter Hints
///
/// ## Parameter name hints — argument names at call sites and constructor calls
///
/// - status: supported
/// - order: 1

void draw(int width, int height);

struct Point {
    Point(int x, int y);
    Point(const Point& other);
    Point(Point&& other);
};

void use() {
    draw(10, 20);
    Point p(1, 2);
    Point q{3, 4};
    // Copy and move constructors stay quiet; a temporary's own braces
    // still hint (the outer prvalue construction is elided anyway).
    Point r(p);
    Point m(Point{5, 6});
    Point s(static_cast<Point&&>(r));
}
