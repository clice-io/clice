import missing;
#define OP(x) x,
enum Op {
#include "ops.def"
};
int main() { return add; }
