// - verify: server
// - indexing: true
//
// A `static` function a shared header defines is a copy per unit, yet
// navigation gathers its uses from every unit including the header.

#include "util.h"

int first(int value) {
    return §(use)helper(value);
}
