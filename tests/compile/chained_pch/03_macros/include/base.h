#pragma once

#define LIMIT 10
#define SQUARE(x) ((x) * (x))
#define MODE 1

inline int first_counter = __COUNTER__;
inline int second_counter = __COUNTER__;

#include "guarded.h"
#include "once.h"

#if __has_include("missing_feature.h")
#define HAS_FEATURE 1
#else
#define HAS_FEATURE 0
#endif
