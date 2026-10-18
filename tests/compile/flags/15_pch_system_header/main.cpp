#include "extra.h"

int run() {
    Point point{1, 2};
    return measure(point) + point.norm();
}
