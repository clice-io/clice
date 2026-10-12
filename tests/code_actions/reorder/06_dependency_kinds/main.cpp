// Moving each class's second definition ahead of its first would put it
// before something it relies on: a using-directive, an include, a macro
// still defined, a constant expression's definition, a complete type, a
// forward declaration. Both stay in place.

namespace lib {
inline int helper() {
    return 1;
}
}  // namespace lib

constexpr int answer();
struct Pending;

struct §(using)U {
    int a();
    int b();
};

int U::b() {
    return 0;
}

using namespace lib;

int U::a() {
    return helper();
}

struct §(include)I {
    int a();
    int b();
};

int I::b() {
    return 0;
}

#include "support.h"

int I::a() {
    return 0;
}

#define LIMIT 2

struct §(undef)D {
    int a();
    int b();
};

int D::b() {
    return LIMIT;
}

#undef LIMIT

int D::a() {
    return 0;
}

struct §(constexpr)K {
    int a();
    int b();
};

int K::b() {
    return 0;
}

constexpr int answer() {
    return 42;
}

int K::a() {
    static_assert(answer() == 42);
    return 0;
}

struct §(complete)T {
    int a();
    int b();
};

int T::b() {
    return 0;
}

struct Pending {
    int x;
};

int T::a() {
    Pending pending{};
    return pending.x;
}

struct §(forward)F {
    int a();
    int b();
};

int F::b() {
    return 0;
}

struct Later;

int F::a() {
    Later* later = nullptr;
    return later == nullptr;
}

struct Later {};
