struct Point {
    int §(x)x;
    int y;
    union {
        int §(raw)raw;
        float real;
    };
};

struct Origin {
    int x;
    Origin(int start) : §(init)x(start) {}
};

void shift(Point& §(point)point) {
    point.x = point.y;
    point.x += 1;
    point.raw = point.x;
    Point corner{.x = 0, .y = 0};
    corner.real = 1.0f;
}
