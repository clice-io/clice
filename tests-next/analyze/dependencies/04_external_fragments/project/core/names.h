#pragma once

enum ExtNames {
#define NAME(x) x,
#include "names.inc"
#undef NAME
};
