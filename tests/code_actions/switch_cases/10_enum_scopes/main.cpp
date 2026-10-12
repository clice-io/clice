// Enums where everyday code keeps them: one nested in a class is qualified
// through it, one brought in by `using enum` keeps its full name, and an
// empty switch receives every enumerator.

struct Widget {
    enum class Kind { Button, Label, Input };
};

int weight(Widget::Kind kind) {
    §(nested)switch (kind) {
    case Widget::Kind::Button:
        return 1;
    }
    return 0;
}

enum class Color { Red, Green, Blue };

int paint(Color color) {
    using enum Color;
    §(using_enum)switch (color) {
    case Red:
        return 1;
    }
    return 0;
}

int mix(Color color) {
    §(empty)switch (color) {
    }
    return 0;
}
