#pragma once

struct Point {
    int x;
    int y;
};

inline int distance(Point a, Point b) {
    int dx = a.x - b.x;
    int dy = a.y - b.y;
    return dx * dx + dy * dy;
}
