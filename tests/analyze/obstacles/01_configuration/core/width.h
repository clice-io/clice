// Read two ways too, but declaring the same entities either way: no
// obstacle the overview counts.
#pragma once

inline int width() {
#ifdef MODE_A
    return 2;
#else
    return 1;
#endif
}
