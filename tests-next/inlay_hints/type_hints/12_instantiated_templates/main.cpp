void take(int first, int second);

template <typename T>
struct Single {
    void reset() {
        §(single)⟦take(1, 2);
        auto copy = T();⟧
    }
};

template struct Single<char>;

template <typename T>
struct Twice {
    void reset() {
        §(twice)⟦auto copy = T();⟧
    }
};

template struct Twice<char>;
template struct Twice<int>;
