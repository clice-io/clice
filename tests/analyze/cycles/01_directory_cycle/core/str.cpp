#include "core/str.h"
#include "feature/hook.h"

int length(const Str& s) {
    return s.n + hook(nullptr);
}
