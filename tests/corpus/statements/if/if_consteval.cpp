// C++23 if consteval
namespace if_consteval {

constexpr int compute(int x) {
    if consteval {
        return x * x;  // compile-time path
    } else {
        return x + x;  // runtime path
    }
}

// negated form: if !consteval
constexpr int runtime_prefer(int x) {
    if !consteval {
        return x + 1;  // runtime path
    } else {
        return x - 1;  // compile-time path
    }
}

// consteval function callable only at compile time
consteval int ct_only(int x) {
    return x * 3;
}

constexpr int dispatch(int x) {
    if consteval {
        return ct_only(x);  // OK: in immediate context
    } else {
        return x;
    }
}

// if consteval without else
constexpr int maybe_optimize(int x) {
    if consteval {
        return x * x * x;
    }
    return x;
}

static_assert(compute(5) == 25);
static_assert(dispatch(3) == 9);
static_assert(maybe_optimize(4) == 64);

}  // namespace if_consteval
