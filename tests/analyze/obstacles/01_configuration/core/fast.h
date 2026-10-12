// Tests CORE_FAST without including its definition: compiled alone, it
// reads differently, silently.
#pragma once

#ifdef CORE_FAST
inline int fast() {
    return 1;
}
#endif
