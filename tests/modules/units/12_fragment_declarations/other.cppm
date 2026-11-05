module;
#include "legacy.h"
class §(fwd)Forward;
static int offset() {
    return 1;
}
module lib:other;

int thrice(int value) {
    return Legacy{value}.value * 3 + §(use)offset();
}
