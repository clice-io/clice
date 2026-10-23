#pragma once

void target(int first, int second);

template <typename... Args>
void wrap(Args&&... args) {
    target(static_cast<Args&&>(args)...);
}

struct Point {
    Point(int x, int y);
};

template <typename T, typename... Args>
T* create(Args&&... args) {
    return new T(static_cast<Args&&>(args)...);
}

template <typename T>
struct Stack {
    template <typename... Args>
    void emplace(Args&&... args) {
        delete new T(static_cast<Args&&>(args)...);
    }
};
