module;
#include "legacy.h"
class Forward;
static int offset() {
    return 1;
}
module lib:impl;

int twice(int value) {
    return Legacy{value}.value * 2 + offset();
}
