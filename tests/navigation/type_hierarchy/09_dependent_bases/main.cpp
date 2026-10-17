struct §(base)Base {};

struct Other {};

template <typename T>
struct §(mixin)Mixin : T {};

Mixin<Base> mixed;
Mixin<Other> other;
