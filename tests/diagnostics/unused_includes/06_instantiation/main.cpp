#include "point.h"
#include "less.h"
#include "ordered.h"
#include "unused.h"

bool use() {
    return ordered(Point{1}, Point{2});
}
