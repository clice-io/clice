// A field designated in part (`.origin.x = 1`, a C99 form clang accepts in
// C++) may be designated again for another part; one designated whole may
// not, nor may the fields before it.
struct Point {
    int x;
    int y;
};

struct Shape {
    int id;
    Point origin;
    int color;
};

Shape first = { .origin.x = 1, .§(after_part) };
Shape second = { .origin = {}, .§(after_whole) };
