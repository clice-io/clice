#include "shapes/registry.h"
#include "registry_limits.h"

namespace shapes {

static int registered = 0;

int registry_count() {
    return registered;
}

int registry_reset() {
    registered = 0;
    return registry_count();
}

void registry_add(const Shape&) {
    if(registered < registry_limit) {
        registered += 1;
    }
}

}  // namespace shapes
