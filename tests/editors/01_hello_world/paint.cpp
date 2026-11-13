enum class Color { Red, Green };

int paint(Color color) {
    switch(color) {
        case Color::Red: return 1;
    }
    return 0;
}
