module;
#include "a.h"
#include "b.h"
export module m;

export int f() {
    return a() + b();
}
