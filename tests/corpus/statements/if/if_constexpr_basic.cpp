// if constexpr: basic compile-time branching
namespace if_constexpr_basic {

template <typename T>
constexpr int type_rank() {
    if constexpr(__is_same(T, char)) {
        return 1;
    } else if constexpr(__is_same(T, int)) {
        return 2;
    } else if constexpr(__is_same(T, long long)) {
        return 3;
    } else {
        return 0;
    }
}

static_assert(type_rank<char>() == 1);
static_assert(type_rank<int>() == 2);
static_assert(type_rank<long long>() == 3);
static_assert(type_rank<float>() == 0);

// discarded branch not instantiated
template <typename T>
auto dereference(T val) {
    if constexpr(__is_pointer(T)) {
        return *val;
    } else {
        return val;
    }
}

void test() {
    int x = 42;
    [[maybe_unused]] int r1 = dereference(&x);
    [[maybe_unused]] int r2 = dereference(x);
}

}  // namespace if_constexpr_basic
