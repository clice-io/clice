#include "extra.h"

struct Later {};
int helper(Later);

struct Local {};
int helper(Local);

int run() {
    decltype(make(2)) made = §(make)make(3);
    int total = made + wrapped->§(size)size() + §(apply)apply(Local{});
    return total + apply(Gadget{}) + apply(Later{}) + apply(Early{}) + base_use() + top_use();
}

int probe(Early early, Gadget gadget, Later later) {
    return §(early)helper(early) + §(gadget)helper(gadget) + §(later)helper(later);
}
