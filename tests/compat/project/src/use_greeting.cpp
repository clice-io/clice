#include <cstdio>

#if __has_include("use_greeting.cpp.expect.h")
#include "use_greeting.cpp.expect.h"
#endif

import compat.greeting;

int use_greeting() {
    return greeting();
}
