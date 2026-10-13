namespace trailing {
struct Point {
    int x; ///< The horizontal coordinate.
    int y; // The vertical coordinate.
};

enum class Color {
    Red,   ///< The red channel.
    Green, ///< The green channel.
};

int use() {
    Point p{1, 2};
    return p.§(marked)x + p.§(plain)y + static_cast<int>(Color::Gr§(enumerator)een);
}
}
