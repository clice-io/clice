#pragma once
#include <alpha/alpha.h>

namespace core {
inline int size() {
    return kt::Thing{1}.v + kit_local() + alpha::make();
}
}  // namespace core
