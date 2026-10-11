#include "registry.h"

static int registered = 0;

int registry_count() {
    return registered;
}
