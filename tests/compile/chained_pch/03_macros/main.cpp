#include "extra.h"

int value = §(limit)LIMIT + §(mode)MODE + §(square)SQUARE(2) + §(c_extra)EXTRA_ONLY;

#include "guarded.h"
#include "once.h"

static_assert(LIMIT == 10);
static_assert(MODE == 2);
static_assert(HAS_FEATURE == 0);
static_assert(preamble_limit == 20);

int counter_now = __COUNTER__;
static_assert(__COUNTER__ == 4);

Guarded guarded{1};
Once once{2};

int run() {
    return guarded.§(guard)g + once.§(once)o + value + counter_now + §(c_limit)LIMIT;
}
