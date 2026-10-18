#pragma once

#include "base.h"

struct Late {
    int cells[4];
};

struct Gadget {};
int helper(Gadget);

inline int top_use() {
    return apply(Gadget{}) + make(1);
}
