// - verify: server

int choose_literal(bool condition) {
    §(keyword_if)if (condition)
        §(keyword_return)return §(literal_value)42;
    return 0;
}

template <typename T>
using identity = T;

// A function declared through an alias writes no return type of its own.
identity<decltype(§(functional_cast_auto)auto(0))()> declared_through_alias;
