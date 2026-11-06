#include "config.h"
#if USE_FAST
#include "fast.h"
#endif
#include "twice.h"
#include "unused.h"

int use() {
    return TWICE(fast());
}
