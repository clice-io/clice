namespace shapes
{

struct Point {
    int x;
    int y;
};

int area(Point p) {
    if (p.x > 0) {
        return p.x * p.y;
    } else {
        return 0;
    }
}

int scaled(Point p,
           int factor)
{
    return area(p) * factor;
}

}  // namespace shapes

/*
 * Debug helpers.
 */
#ifdef SHAPES_DEBUG
void dump();
#endif

#pragma region Visitors
void visit(void (*callback)(int));
void visit_with(void (*callback)(int), int times);
#pragma endregion

void run() {
    visit([](int value) {
        value += 1;
    });
    visit_with([](int value) {
        value += 1;
    }, 2);
}
