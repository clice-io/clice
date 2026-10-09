#include "shapes/registry.h"

namespace shapes {

static int registered = 0;

int registry_count() {
    return registered;
}

}  // namespace shapes
