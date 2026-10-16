struct Widget {
    int width;
    using Size = int;
};

template <typename T, int N>
T scaled(T value) {
    return value * N;
}
