// util/strops.h serves core alone, and util/fmt.h app alone, with the
// source implementing it and the header that source implements too. Not
// candidates: lib/base.h is all of lib, which would be a merge;
// feature/f.cpp, implementing feature/f.h, names feature/dup.h; and an
// instantiation uses core/traits.h's specialization without naming it.
#include "core/traits.h"
#include "feature/f.h"
#include "lib/base.h"
#include "util/clock.h"
#include "util/fmt.h"

int main() {
    Clock clock{};
    Base base{};
    return format_str(clock.name) + base.b + feature_hook() + Traits<Str>::value;
}
