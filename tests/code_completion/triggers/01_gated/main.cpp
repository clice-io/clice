// Trigger characters that open no completion here answer an empty list: a
// space after a complete include or inside a statement, a `<` opening a
// template parameter list and the `>` closing it, the dots of a pack typed
// or half typed, a `.` in a number, a string or a comment, and the `>` of a
// decrement.
#include "box.h" §(include_space)
#define UNRELATED_MACRO 123

int value() {
    return §(statement_space)0;
}

template <§(template_angle)typename T>§(template_close)
struct Holder {};

template <typename...§(ellipsis_dot)Ts>
struct Pack {};

template <typename..§(half_ellipsis) Rest>
struct Typing {};

double number = 3.§(number_dot);
double hex = 0x1F.§(hex_dot);
const char* text = "a.§(string_dot)";
// a.§(comment_dot)

int drain(int x) {
    while(x-->§(decrement)0) {
    }
    return x;
}
