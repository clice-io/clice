struct Point {
    int x;
    int y;
};

int norm(Point p) { return p.x * p.x + p.y * p.y; }

int main() {
    Point p = {.x = 1, .y = 2};
    Point q{.x = 3};
    return norm(p) + q.x;
}
