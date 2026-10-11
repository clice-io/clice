namespace evaluated_context {
constexpr long cube(long value) {
    return value * value * value;
}
long §(constexpr_call)from_call = cube(3);

long §(sizeof)from_size = sizeof(double);

enum class State { Offline = -2, Online = 6 };
State selected = State::§(enumerator)Online;

template <long Left, long Right> struct Product {
    static constexpr long value = Left * Right;
};
long §(static_member)from_member = Product<4, 5>::value;
}
