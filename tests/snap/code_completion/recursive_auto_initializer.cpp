// - diagnostics: expected

// An `auto` static member initialized from another specialization of itself,
// or from a second template's member initialized back from it, deduces
// nothing; completing on a variable initialized from it completes nothing.
template <int N>
struct Depth {
    static constexpr auto value = Depth<N - 1>::value;
};

template <typename T>
struct Ping;

template <typename T>
struct Pong {
    static inline auto value = Ping<T>::value;
};

template <typename T>
struct Ping {
    static inline auto value = Pong<T>::value;
};

template <int N, typename T>
void bar() {
    auto depth = Depth<N>::value;
    depth.§(self);
    auto ping = Ping<T>::value;
    ping.§(mutual);
}
