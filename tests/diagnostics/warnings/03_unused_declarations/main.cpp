#include "compare.h"

namespace {

struct Point {
    int x;
};

bool operator==(const Point& a, const Point& b) {
    return a.x == b.x;
}

void forgotten() {}

}  // namespace

bool check() {
    return same(Point{1}, Point{1});
}
