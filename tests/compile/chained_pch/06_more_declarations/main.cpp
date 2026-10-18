#include "extra.h"

static_assert(Measurable<Ruler>);
static_assert(!Measurable<Plain>);

int run() {
    Plain plain{1};
    §(plain)Plain other = plain;
    Token token{2};
    Wrap §(wrap)wrapped{"text"};
    Cell<int> cell{3};
    §(item)Item item{4};
    §(legacy)legacy();
    int total = §(peek)peek(token) + ::§(qualified)peek(token) + cell.§(cell)get();
    return total + item.§(weight)weight + other.§(copied)a + copy_plain(plain).a;
}
