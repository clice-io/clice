#include "missing.h"
#define OP(x) x,
enum Op {
#include "ops.def"
};
int main() { return add; }
