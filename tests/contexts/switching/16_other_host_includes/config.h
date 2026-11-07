#pragma once

inline int base() {
    return 0;
}

#ifdef FAST
#include "fast.h"
#else
#include "slow.h"
#endif
