module;
#include "legacy.h"
class §(fwd)Forward;
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
constexpr int area = §(width)width * width;
module lib:other;

int thrice(int value) {
    return Legacy{value}.value * 3 + §(use)offset();
}
