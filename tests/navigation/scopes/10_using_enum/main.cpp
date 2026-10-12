// `using enum` names the enum, and the enumerators it brings into scope
// stay its own.

enum class Color { Red, Green };

int pick() {
    using enum §(enum_name)Color;
    return static_cast<int>(§(enumerator)Red);
}
