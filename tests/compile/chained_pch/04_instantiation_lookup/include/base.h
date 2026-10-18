#pragma once

inline int helper(...) {
    return 0;
}

template <class T>
int apply(T value) {
    return helper(value);
}

template <class T>
auto make(T value) {
    return value;
}

template <class T>
struct Wrapper {
    T* pointer;

    int size() const {
        return sizeof(T);
    }
};

struct Late;
inline Wrapper<Late>* wrapped = nullptr;

struct Early {};
int helper(Early);

inline int base_use() {
    return apply(Early{});
}
