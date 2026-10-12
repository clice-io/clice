#include "ops.h"
const char* names[] = {
#define OP(x) #x,
#include "ops.def"
#undef OP
};
int main() { return Add; }
