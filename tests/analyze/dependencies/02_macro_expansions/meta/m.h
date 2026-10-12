// meta names Clock only in a macro body it expands itself; app only
// through util's macro.
#pragma once
#include "util/clock.h"

#define MAKE_CLOCK() Clock{}

inline int meta_size() {
    return MAKE_CLOCK().size();
}
