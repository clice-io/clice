// No interface imports lib, but main.cpp reaches it through this internal
// header.
#pragma once
#include "lib/base.h"

inline int base_size() {
    return sizeof(Base);
}
