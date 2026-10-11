// Marker-free canary over statement and expression syntax: the snapshot
// pins every token, so a stray token on any punctuation or operator fails
// the suite even where nobody thought to place a marker. The attribute
// lines also pin that attribute names currently produce no tokens.

enum class Color { Red };

struct Shape {
    int sides;
};

int area(int width, int height) {
    return width * height;
}

void test() {
    Shape square{4};
    Color c = Color::Red;
    [[maybe_unused]] int a = area(2 * 2, square.sides);
    [[maybe_unused]] bool red = c == Color::Red;
}
