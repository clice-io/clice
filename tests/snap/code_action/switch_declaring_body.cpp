// A switch body declaring a variable receives the missing cases at its top:
// above a conditional directive around the first label, and above a first
// label carrying an attribute, which falls through into the next section.

enum class Shape { Circle, Square, Triangle };

int sides(Shape shape) {
    §(conditional)switch (shape) {
#if 1
    case Shape::Circle:
        return 0;
#endif
    case Shape::Square:
        int count = 4;
        return count;
    }
    return 3;
}

int corners(Shape shape, int extra) {
    §(attributed)switch (shape) {
    [[likely]] case Shape::Circle:
        extra += 1;
        [[fallthrough]];
    case Shape::Square:
        int count = 4;
        return count + extra;
    }
    return 3;
}
