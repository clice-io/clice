#pragma once

#include "base.h"

struct Widget {
    int alpha_member;
};

inline int pre() {
    return base();
}
