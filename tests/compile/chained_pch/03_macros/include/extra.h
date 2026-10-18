#pragma once

#include "base.h"
#include "guarded.h"
#include "once.h"

#undef MODE
#define MODE 2

#pragma push_macro("LIMIT")
#undef LIMIT
#define LIMIT 20
inline constexpr int preamble_limit = LIMIT;
#pragma pop_macro("LIMIT")

inline int third_counter = __COUNTER__;

#define EXTRA_ONLY 5
