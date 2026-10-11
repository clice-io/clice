#pragma once
namespace core {
namespace {
inline int hidden() { return 1; }
}
inline int detail() { return hidden(); }
}
#include "cfg/late.h"
#ifdef CFG_FAST
#include "cfg/extra.h"
#endif
