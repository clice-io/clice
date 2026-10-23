#include "compare.h"

namespace {

struct Point {
    int x;
};

bool §(equal)operator==(const Point& a, const Point& b) {
    return a.x == b.x;
}

}  // namespace

bool check() {
    return same(Point{1}, Point{1});
}
