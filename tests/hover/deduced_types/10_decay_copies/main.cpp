// `auto(x)` and `auto{x}` copy a value as a prvalue of its decayed type.

namespace decay_copies {

const long limits[2] = {1, 2};

auto first = §(parenthesized)auto(limits);
auto second = §(braced)auto{limits[0]};

}
