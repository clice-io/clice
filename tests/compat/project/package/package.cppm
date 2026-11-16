module;

#include <compat_package/detail.h>
#include <cstdio>

#if __has_include("package.cppm.expect.h")
#include "package.cppm.expect.h"
#endif

export module compat.package;

export namespace compat {

int packaged() {
    return detail::base() + 1;
}

}  // namespace compat
