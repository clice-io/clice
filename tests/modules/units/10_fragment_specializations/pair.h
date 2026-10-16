#pragma once

#include "tuple.h"

namespace lib {

template <class T>
class Pair {
    int index = 0;
    T value{};

public:
    template <decltype(sizeof 0) I>
    auto get() const {
        if constexpr(I == 0) {
            return index;
        } else {
            return value;
        }
    }
};

template <class T>
Pair<T> make(T) {
    return {};
}

}  // namespace lib

template <class T>
struct std::tuple_size<lib::Pair<T>> {
    static constexpr decltype(sizeof 0) value = 2;
};

template <decltype(sizeof 0) I, class T>
struct std::tuple_element<I, lib::Pair<T>> {
    using type = decltype(lib::Pair<T>().template get<I>());
};
