#include <cstdio>
#include <string>
#include <vector>

#if __has_include("main.cpp.expect.h")
#include "main.cpp.expect.h"
#endif

#include "util.h"

#if __has_include(<omp.h>)
#include <omp.h>
#endif
// clang has no __float128 on some targets GCC has it on (Darwin, Cygwin).
#if __has_include(<quadmath.h>) && defined(__SIZEOF_FLOAT128__)
#include <quadmath.h>
#endif
#if __has_include(<openacc.h>)
#include <openacc.h>
#endif
#if __has_include(<ptrcheck.h>)
#include <ptrcheck.h>
#endif
#if __has_include(<sanitizer/memprof_interface.h>)
#include <sanitizer/memprof_interface.h>
#endif
#if __has_include(<xray/xray_interface.h>)
#include <xray/xray_interface.h>
#endif

#ifndef COMPAT_DEFINE
#error "the build's own define is missing"
#endif

int main() {
    std::vector<std::string> names{"a", "b"};
    std::printf("%d %zu\n", util_add(1, 2), names.size());
    return 0;
}
