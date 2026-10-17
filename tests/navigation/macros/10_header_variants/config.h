#pragma once

#ifdef FAST
#define MODE 1
#else
#define MODE 2
#endif

inline int mode() {
    return §(use)MODE;
}
