#include "widget.h"

template <typename T, typename U>
struct Pair {};

Pair<Widget, Gadget> make_pair();
const Widget* find(int id);

void use() {
    auto pair = make_pair();
    auto found = find(1);
    resize(2);
}
