#pragma once

#define LIMIT 8

inline int configured() {
#if FAST
    return LIMIT;
#else
    return LIMIT * 2;
#endif
}

inline int plain() { return 2; }
