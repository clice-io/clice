/// # Fields and member access
///
/// - status: supported
///
/// Writing through `object.member` writes the member, while the object is
/// only read
///
/// A designated initializer or a constructor's member initializer names the
/// field it initializes without writing it: initialization is not an
/// assignment. Fields of an anonymous union highlight like any other
/// field.

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
