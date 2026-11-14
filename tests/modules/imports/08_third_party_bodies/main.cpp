#include "common.h"

import lib;

int helper(int x) {
    return x;
}

int run() {
    return size_of(42) + twice(helper(1));
}
