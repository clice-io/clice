#include <cstdio>

#if __has_include("use_package.cpp.expect.h")
#include "use_package.cpp.expect.h"
#endif

import compat.package;

int use_package() {
    return compat::packaged();
}
