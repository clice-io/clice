module;
#include "config.h"
export module A;
#if USE_B
import B;
#endif

export int a() {
#if USE_B
    return b();
#else
    return 1;
#endif
}
