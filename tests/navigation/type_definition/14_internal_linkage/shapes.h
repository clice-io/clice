#pragma once

namespace {
struct Point {
    int x;
};

struct Pair {
    Point first;
    int second;
};

Pair make_pair() {
    return {};
}
}
