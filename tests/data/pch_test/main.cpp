#include "common.h"

int add(int a, int b) {
    return a + b;
}

int main() {
    Point p{1, 2};
    int result = add(p.x, p.y);
    return result;
}
