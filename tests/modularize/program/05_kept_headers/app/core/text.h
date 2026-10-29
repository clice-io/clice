#pragma once
#include <alpha/alpha.h>

namespace ext {
enum class Status : alpha::Code;
}

namespace core {
inline int size() {
    return kt::Thing{1}.v + kit::Gizmo{2}.v + kit_local() + alpha::make();
}
}  // namespace core
