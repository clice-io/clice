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
        T value{};
        §(twice)⟦auto copy = value;⟧
    }
};

template struct Twice<char>;
template struct Twice<int>;

template <typename T>
struct Outside {
    void reset();
};

template <typename T>
void Outside<T>::reset() {
    §(outside)⟦auto copy = T();⟧
}

template struct Outside<char>;
