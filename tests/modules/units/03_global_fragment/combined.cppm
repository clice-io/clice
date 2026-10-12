module;
#include "legacy.h"
export module Combined;
import Base;

export int combined() {
    return §(included)legacy_fn() + §(imported)base();
}
