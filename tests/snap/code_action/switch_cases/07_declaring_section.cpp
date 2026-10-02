/// # Sections declaring variables
///
/// - status: supported
///
/// Without a `default`, a switch declaring a variable at its own scope receives the missing cases before its first label, since a label after the declaration would jump past it
///
/// No section falls through into cases placed there.

enum class Shape { Circle, Square, Triangle, Hexagon };

int sides(Shape shape) {
    int extra = 0;
    §(declares)switch (shape) {
    case Shape::Circle:
        extra = 1;
        [[fallthrough]];
    case Shape::Square:
        int count = 4;
        return count + extra;
    }
    return 3;
}
