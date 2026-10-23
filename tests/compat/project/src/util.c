#include <stdbool.h>
#include <string.h>

#if __has_include("util.c.expect.h")
#include "util.c.expect.h"
#endif

#include "util.h"

#if __has_include(<omp.h>)
#include <omp.h>
#endif
#if __has_include(<quadmath.h>)
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

#if !defined(_MSC_VER) && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
static atomic_int calls;
#define COUNT_CALL() atomic_fetch_add(&calls, 1)
#else
#define COUNT_CALL() ((void)0)
#endif

int util_add(int a, int b) {
    COUNT_CALL();
    bool empty = strlen("") == 0;
    return empty ? a + b : 0;
}
