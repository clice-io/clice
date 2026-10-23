#include "compare.h"

namespace {

struct Point {
    int x;

    bool equals(const Point& other) const;

    int forgotten_member() const;
};

bool Point::equals(const Point& other) const {
    return x == other.x;
}

int Point::forgotten_member() const {
    return x;
}

bool operator==(const Point& a, const Point& b) {
    return a.x == b.x;
}

void forgotten() {}

void silenced() {}  // NOLINT

}  // namespace

bool check() {
    return same(Point{1}, Point{1}) && equal(Point{1}, Point{2});
}
