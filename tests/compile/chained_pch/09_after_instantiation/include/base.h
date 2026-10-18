#pragma once

template <class T>
struct Traits {
    static constexpr int value = 0;
};

template <class T>
int pick(T) {
    return 0;
}

template <class T>
constexpr int weight = 1;

struct Unit {};

inline int base_use() {
    return Traits<int*>::value + Traits<Unit>::value + pick(Unit{}) + weight<Unit>;
}
