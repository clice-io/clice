module;

#include <cstdio>

#if __has_include("greeting.cppm.expect.h")
#include "greeting.cppm.expect.h"
#endif

export module compat.greeting;

export int greeting() {
    return 42;
}
