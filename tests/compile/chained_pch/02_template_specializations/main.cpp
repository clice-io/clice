#include "extra.h"

static_assert(Traits<Widget>::value == 7);
static_assert(Traits<int*>::value == 3);
static_assert(weight<Widget> == 9);

int run() {
    Box<Widget> box{};
    Box<Inbase> other{};
    int* pointer = nullptr;
    Holder<Widget> holder;
    int total = box.§(get)get() + other.§(get_base)get() + §(twice)twice(pointer);
    total += §(describe)describe(Widget{}) + Traits<Widget>::§(value)value + holder.§(size)size();
    total += §(weight)weight<Widget> + base_use() + top_use() + twice(Inbase{});
    return total + Traits<int*>::§(partial)value;
}
