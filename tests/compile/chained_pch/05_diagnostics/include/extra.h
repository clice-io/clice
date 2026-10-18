#pragma once

#include "base.h"

inline int broken() {
    return undefined_in_preamble;
}

[[deprecated]] inline int old_extra() {
    return 1;
}
