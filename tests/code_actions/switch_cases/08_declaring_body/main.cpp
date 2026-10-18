// A switch body declaring a variable receives the missing cases at its top:
// above a conditional directive around the first label, above a first
// label carrying an attribute, which falls through into the next section,
// and below a comment opening a body in digraph braces.

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

int digraphs(Shape shape) {
    §(digraph)switch (shape) <%
    // the round one
    case Shape::Circle:
        int count = 0;
        return count;
    %>
    return 3;
}
