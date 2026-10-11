// Each category of hints turns off on its own, and `enabled` turns them all
// off. The default shows each category's kind and padding.
struct Point {
    int x;
    int y;
};

int scale(int factor, int offset = 0);

auto twice(int value) {
    return value * 2;
}

void use() {
    Point origin{0, 0};
    auto doubled = twice(4);
    int scaled = scale(3);
}
