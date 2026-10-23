#pragma once

namespace std {

template <typename T>
struct remove_reference {
    using type = T;
};

template <typename T>
constexpr T&& forward(typename remove_reference<T>::type& value) noexcept;

}  // namespace std

void target(int first, int second);

template <typename... Args>
void wrap(Args&&... args) {
    target(std::forward<Args>(args)...);
}

struct Point {
    Point(int x, int y);
};

template <typename T, typename... Args>
T* create(Args&&... args) {
    return new T(std::forward<Args>(args)...);
}

template <typename T>
struct Stack {
    template <typename... Args>
    void emplace(Args&&... args) {
        delete new T(std::forward<Args>(args)...);
    }
};
