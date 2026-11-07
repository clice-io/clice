#pragma once

#ifdef FAST
#pragma mark - Fast

inline int §(fast)fast_only() {
    return 1;
}

struct Timer {
    ~Timer();
};
#endif
