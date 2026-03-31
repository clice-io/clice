module;
#include "legacy.h"
export module GMF;

export int wrapped() {
    return legacy_fn();
}
