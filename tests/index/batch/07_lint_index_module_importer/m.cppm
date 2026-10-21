module;
#include "h.h"
export module m;
export int from_module(int x) { return helper(x) + 1; }
export struct ModuleThing { int a; int b; };
