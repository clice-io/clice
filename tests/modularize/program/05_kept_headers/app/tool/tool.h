#pragma once
#include <alpha/alpha.h>
#include "core/text.h"

namespace tool {
inline int run() {
    return core::size() + kit_local();
}
}  // namespace tool
