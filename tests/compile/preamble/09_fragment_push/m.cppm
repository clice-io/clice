// The global module fragment pushes min around its include and the purview
// pops it: a preamble ending at the push would leave the include to the
// body, ahead of the module declaration, so the file compiles without one.
module;
#pragma push_macro("min")
#include "legacy.h"
export module m;
#pragma pop_macro("min")

export int f() {
    return legacy();
}
