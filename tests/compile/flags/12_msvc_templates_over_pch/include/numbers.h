#pragma once

inline int one() {
    return 1;
}

template <class T>
T twice(T value) {
    return value + value;
}

template <class T>
T thrice(T value) {
    return value + value + value;
}

template <class T>
T plus_one(T value) {
    return value + one();
}
