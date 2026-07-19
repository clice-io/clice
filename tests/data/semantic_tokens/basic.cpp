// Seed case for the per-feature snapshot corpus: basic token kinds.
#define SQUARE(x) ((x) * (x))

namespace demo {

enum class Color { Red, Green };

struct Shape {
    int sides;
};

int area(int width, int height) {
    return width * height;
}

void test() {
    Shape square{4};
    Color c = Color::Red;
    [[maybe_unused]] int a = area(SQUARE(2), square.sides);
    [[maybe_unused]] bool red = c == Color::Red;
}

}  // namespace demo
