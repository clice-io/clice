/// # Constant evaluation
///
/// - status: supported
///
/// Constexpr, enumerators, sizeof
///
/// When an initializer is a constant expression, the card evaluates it and
/// shows the resulting value.

namespace evaluated_context {
constexpr long cube(long value) {
    return value * value * value;
}
long §(01_constexpr_call)from_call = cube(3);

long §(02_sizeof)from_size = sizeof(double);

enum class State { Offline = -2, Online = 6 };
State selected = State::§(03_enumerator)Online;

template <long Left, long Right> struct Product {
    static constexpr long value = Left * Right;
};
long §(04_static_member)from_member = Product<4, 5>::value;
}
