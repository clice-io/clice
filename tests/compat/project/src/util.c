#include <stdbool.h>
#include <string.h>

#if __has_include("util.c.expect.h")
#include "util.c.expect.h"
#endif

#include "util.h"

int util_add(int a, int b) {
    bool empty = strlen("") == 0;
    return empty ? a + b : 0;
}
