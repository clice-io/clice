// Constructor calls show parameter names as function calls do; copy and
// move constructors stay quiet.
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
