#pragma once
#include <alpha/alpha.h>
#include <alpha/local.h>
#include <fassert.h>
inline int beta_local() { return alpha_local(); }
namespace beta {
inline alpha::Thing wrap(int v) { return alpha::Thing{ALPHA_TWICE(v)}; }
#define BETA_COUNTER(name) ALPHA_EXTERN int name;
#include "counters.inc"
#undef BETA_COUNTER
}
