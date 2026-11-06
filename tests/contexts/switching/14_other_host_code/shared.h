#pragma once

#ifdef FAST
inline int fast_path() {
    return LIMIT;
}
#else
#include "scale.h"

inline int §(slow)slow_path() {
    return LIMIT * SCALE;
}
#endif
