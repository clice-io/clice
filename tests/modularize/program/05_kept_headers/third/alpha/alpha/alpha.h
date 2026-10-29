#pragma once
#include <kit/kit.h>

namespace alpha {
using Code = int;

inline int make() {
    return kit::Thing{1}.v;
}
}  // namespace alpha
