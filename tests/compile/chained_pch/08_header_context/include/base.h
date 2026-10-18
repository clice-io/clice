#pragma once

namespace geo {

struct Point {
    int x;
    int y;
};

inline int scale(int value, int factor = 2) {
    return value * factor;
}

}  // namespace geo
