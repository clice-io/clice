// The global module fragment pushes min around its include and the purview
// pops it: the preamble cannot end at the push, inside the fragment.
module;
#pragma push_macro("min")
#include "legacy.h"
export module m;
#pragma pop_macro("min")

export int f() {
    return legacy();
}
