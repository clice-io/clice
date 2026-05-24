// if constexpr with requires-expression
namespace if_constexpr_requires {

template <typename T>
int get_length(const T& x) {
    if constexpr(requires { x.size(); }) {
        return static_cast<int>(x.size());
    } else if constexpr(requires { x.length; }) {
        return x.length;
    } else {
        return 1;
    }
}

struct WithLength {
    int length;
};

struct Container {
    int data[4];

    int size() const {
        return 4;
    }
};

void test() {
    Container c{};
    [[maybe_unused]] int r1 = get_length(c);

    WithLength wl{10};
    [[maybe_unused]] int r2 = get_length(wl);

    [[maybe_unused]] int r3 = get_length(42);
}

}  // namespace if_constexpr_requires
