#pragma once

struct Point {
    int x;
    int y;
};

inline int norm(Point p) {
    return p.x + p.y;
}
