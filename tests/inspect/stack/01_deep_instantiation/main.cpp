template <int N>
struct Depth {
    static constexpr int value = Depth<N - 1>::value + 1;
};

template <>
struct Depth<0> {
    static constexpr int value = 0;
};

static_assert(Depth<1000>::value == 1000);
