#pragma once
enum Op {
#define OP(x) x,
#include "ops.def"
#undef OP
};
