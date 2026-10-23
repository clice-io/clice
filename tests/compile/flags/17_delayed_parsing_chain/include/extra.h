#pragma once

#include "base.h"

struct Item {
    int value;
};

inline auto use_base() {
    return base_template(Item{1});
}

template <class T>
int top_template(T t) {
    return t.value + base_template(t);
}
