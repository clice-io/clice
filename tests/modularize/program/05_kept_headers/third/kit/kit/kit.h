#pragma once
#include <kit/local.h>

namespace original {
struct Gizmo {
    int v;
};
}  // namespace original

namespace kit {
struct Thing {
    int v;
};
using original::Gizmo;
}  // namespace kit

namespace kt = kit;
