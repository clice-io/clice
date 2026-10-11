enum class Color { Red, Green, Blue, Azure = Blue };

int paint(Color color) {
    §(sw)switch (color) {
    case Color::Red:
        return 1;
    }
    return 0;
}
