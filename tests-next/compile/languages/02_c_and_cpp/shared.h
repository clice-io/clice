#pragma once

#ifdef __cplusplus
inline int language() {
    return 2;
}
#else
static inline int language(void) {
    return 1;
}
#endif
