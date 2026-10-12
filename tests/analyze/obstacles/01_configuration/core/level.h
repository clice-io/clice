// Expands CORE_FAST without including its definition: compiled alone, it
// fails.
#pragma once

inline int level() {
    return CORE_FAST;
}
