#include "shapes.h"

int main() {
    Circle unit{{1, 2}, 1.0};
    return manhattan(unit.center) + (area(unit) > 3 ? 0 : 1);
}
