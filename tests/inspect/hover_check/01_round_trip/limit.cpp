template <int Size> struct Doubled {
    static constexpr int capacity = Size * 2;
};

Doubled<4> doubled;
