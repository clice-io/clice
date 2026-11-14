module;

#include "../common.h"

export module lib;

export template <typename T>
int size_of(T value) {
    return value.size();
}

export int twice(int x) {
    return helper(x) + helper(x);
}
