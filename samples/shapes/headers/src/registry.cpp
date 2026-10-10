#include "shapes/registry.h"

namespace shapes {

static int registered = 0;

int registry_count() {
    return registered;
}

int registry_reset() {
    registered = 0;
    return registry_count();
}

}  // namespace shapes
