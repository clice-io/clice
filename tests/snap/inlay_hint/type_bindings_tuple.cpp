/// # Type Hints
///
/// ## Tuple-protocol bindings — hints print the canonical type, not `tuple_element<I, T>::type`
///
/// - status: supported
/// - order: 11

struct IntPair {
    int a;
    int b;
};

namespace std {

template <typename T>
struct tuple_size {};

template <>
struct tuple_size<IntPair> {
    constexpr static unsigned value = 2;
};

template <unsigned I, typename T>
struct tuple_element {};

template <unsigned I>
struct tuple_element<I, IntPair> {
    using type = int;
};

}  // namespace std

template <unsigned I>
int get(const IntPair& p) {
    if constexpr(I == 0) {
        return p.a;
    } else {
        return p.b;
    }
}

IntPair make();

auto [x, y] = make();
