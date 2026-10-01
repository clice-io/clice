/// # Sections declaring variables
///
/// - status: supported
///
/// Without a `default`, the missing cases go before the first section declaring a variable at the switch's scope, since a label after it would jump past the declaration

enum class Shape { Circle, Square, Triangle, Hexagon };

int sides(Shape shape) {
    §(declares)switch (shape) {
    case Shape::Circle:
        return 0;
    case Shape::Square:
        int count = 4;
        return count;
    }
    return 3;
}
