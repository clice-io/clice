#include "api.h"

struct Point {
    int x;
    int y;
};

int twice(int value) {
    int doubled = value * 2;
    return doubled;
}

namespace geo {
int area(Point p) {
    return p.x * p.y;
}
}
