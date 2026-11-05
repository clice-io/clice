module;
#include "legacy.h"
class Forward;
#if 0
struct Hidden {
    int x;
};
#endif
namespace detail {
int helper();
}
static int offset() {
    return 1;
}
constexpr int width = 4;
constexpr int area = width * width;
module lib:impl;

int twice(int value) {
    return Legacy{value}.value * 2 + offset();
}
