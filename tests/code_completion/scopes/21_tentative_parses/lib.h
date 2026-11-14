#pragma once

struct Global {};

namespace lib {

struct Value {
    int amount;
    int total() const;
};

template <class T>
struct holder {
    using type = T;
};

}  // namespace lib
