#pragma once
#include "util/clock.h"

#define MAKE_CLOCK() Clock{}

inline int meta_size() {
    return MAKE_CLOCK().size();
}
