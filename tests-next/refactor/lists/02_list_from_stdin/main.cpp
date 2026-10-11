#include "lib.h"

int compute(int x) { return x; }

Widget::Widget() : value(0) {}

int use() { return compute(2) + CALL; }

int main() {
    Widget w;
    return use() + w.value;
}
