#pragma once

struct Plain {
    int a;
};

struct Token {
    int id;

    friend int peek(Token token) {
        return token.id;
    }
};

template <class T>
struct Wrap {
    T value;
};

void legacy();

template <class T>
struct Cell {
    T value;

    T get() const {
        return value;
    }
};

extern template struct Cell<int>;

template <class T>
concept Measurable = requires(T t) { t.measure(); };

namespace lib {
inline namespace v1 {
struct Item;
}
}  // namespace lib
