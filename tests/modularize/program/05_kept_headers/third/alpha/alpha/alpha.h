#pragma once
#include <kit/kit.h>

namespace alpha {
inline int make() {
    return kit::Thing{1}.v;
}
}  // namespace alpha
