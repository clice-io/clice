// if constexpr with variadic templates
namespace if_constexpr_variadic {

// recursive fold with if constexpr
template <typename T, typename... Rest>
constexpr T sum(T first, Rest... rest) {
    if constexpr(sizeof...(rest) == 0) {
        return first;
    } else {
        return first + sum(rest...);
    }
}

static_assert(sum(1) == 1);
static_assert(sum(1, 2, 3) == 6);
static_assert(sum(1, 2, 3, 4, 5) == 15);

// process each element differently
template <typename T>
constexpr int count_one() {
    if constexpr(__is_integral(T)) {
        return 1;
    } else {
        return 0;
    }
}

template <typename... Ts>
constexpr int count_integrals() {
    return (count_one<Ts>() + ...);
}

static_assert(count_integrals<int, float, char, double>() == 2);

}  // namespace if_constexpr_variadic
