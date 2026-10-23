#include "wrappers.h"

void use() {
    wrap(1, 2);
    create<Point>(3, 4);
    Stack<Point>{}.emplace(5, 6);
}
