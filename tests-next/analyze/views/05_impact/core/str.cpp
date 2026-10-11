#include "core/detail.h"
#include "core/str.h"

int length(const Str& s) {
    return s.n + detail_value();
}
