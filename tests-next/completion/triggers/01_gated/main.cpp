// Trigger characters that open no completion here answer an empty list: a
// space after a complete include or inside a statement, a `<` opening a
// template parameter list, the dots of a pack.
#include "box.h" §(include_space)
#define UNRELATED_MACRO 123

int value() {
    return §(statement_space)0;
}

template <§(template_angle)typename T>
struct Holder {};

template <typename...§(ellipsis_dot)Ts>
struct Pack {};
