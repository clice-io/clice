template <typename T>
struct Holder {
    T value;
};

template <>
struct Holder<bool> {
    bool flag;
};

Holder<int> numbers;
Holder<bool> flags;
