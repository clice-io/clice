#pragma once
#include <alpha/alpha.h>
#include "cfg/config.h"
#define CORE_TWICE(x) ((x) * 2)
#if CFG_FAST
#define CORE_FAST 1
#endif
#pragma push_macro("CFG_FAST")
#undef CFG_FAST
#pragma pop_macro("CFG_FAST")
namespace core {
struct Sink;
struct Text {
    alpha::Thing thing;
    int size() const;
};
template <typename T>
struct Box { T value; };
template <typename T, typename U>
struct Pair { T first; U second; };
namespace inner { struct Deep {}; }
int hook();
}
