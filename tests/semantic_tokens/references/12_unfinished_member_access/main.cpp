// While a member access is still being typed, the names around it keep their
// colors, the object before the dot included.
struct Point {
    int x;
};

int read(Point point) {
    Point §(declared)copy = point;
    return §(object)copy.
}

int §(after)after = 1;
