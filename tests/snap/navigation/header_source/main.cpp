/// - verify: server

#include §(include_target)"lib.h"

int floor_plan(int width, int height) {
    return §(use)area(width, height) + §(inline_use)perimeter(width, height);
}
