#define X(name) int name;
int include(int value);
#include §(late)"foo.h"
#include "list.def"
#undef X

int use(int value) {
    return §(call)include(value) + alpha + foo();
}
