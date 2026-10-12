#include "core/str.h"
#include "util/strops.h"

int length(const Str& s) {
    return s.n;
}

int doubled(const Str& s) {
    return twice(s);
}
