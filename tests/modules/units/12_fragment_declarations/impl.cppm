module;
#include "legacy.h"
class Forward;
module lib:impl;

int twice(int value) {
    return Legacy{value}.value * 2;
}
