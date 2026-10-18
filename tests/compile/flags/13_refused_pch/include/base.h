#pragma once

#if LEVEL == 2 || __cplusplus > 202002L
constexpr int level() {
    return 2;
}
#else
constexpr int level() {
    return 1;
}
#endif
