// A designator names the field it initializes.

struct Point {
    int x;
    int y;
};

Point make() {
    return Point{.§(field)x = 1, .y = 2};
}

void fill(Point& p) {
    p = {.§(braced)x = 3};
}
