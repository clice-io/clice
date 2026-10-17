#include "box.h"

void Box::open() {}

void reset() {}

template <typename T>
void twice(T& item, Box& box) {
    item.open();
    box.open();
}

void run(Box& box) {
    twice(box, box);
}
