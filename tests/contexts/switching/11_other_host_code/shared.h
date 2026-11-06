#pragma once

#ifdef FAST
inline int fast_path() {
    return LIMIT;
}
#else
inline int §(slow)slow_path() {
    return LIMIT * 2;
}
#endif
