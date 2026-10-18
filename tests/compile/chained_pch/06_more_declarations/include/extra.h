#pragma once

#include "base.h"

inline Plain copy_plain(const Plain& plain) {
    Plain copy = plain;
    return copy;
}

int peek(Token token);

struct Name {
    Name(const char*) {}
};

Wrap(const char*) -> Wrap<Name>;

[[deprecated("gone")]] void legacy();

template struct Cell<int>;

struct Ruler {
    int measure() const {
        return 1;
    }
};

namespace lib {
inline namespace v1 {
struct Item {
    int weight;
};
}  // namespace v1
}  // namespace lib

using namespace lib;
