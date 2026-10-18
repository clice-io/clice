#pragma once

template <class T>
struct Traits {
    static constexpr int value = 0;
};

template <class T>
struct Box {
    T item;

    int get() const {
        return Traits<T>::value;
    }
};

template <class T>
int twice(T) {
    return 2 * Traits<T>::value;
}

template <class T>
int describe(T) {
    return 0;
}

template <class T>
constexpr int weight = 1;

template <class T>
struct Holder {
    int size() const {
        return 1;
    }
};

struct Inbase {};

inline int base_use() {
    return twice(Inbase{}) + Box<Inbase>{}.get() + describe(1) + weight<Inbase>;
}
