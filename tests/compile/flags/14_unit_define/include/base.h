#pragma once

#ifdef FAST_PATH
constexpr int level() {
    return 2;
}
#else
constexpr int level() {
    return 1;
}
#endif
