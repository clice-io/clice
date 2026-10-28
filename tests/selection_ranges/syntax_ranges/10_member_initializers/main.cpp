// A member initializer steps from its argument to the initializer, then to the
// whole constructor.
struct Point {
    int x;
    int y;

    Point(int a, int b) : x(§(init)a), y(b) {}
};
