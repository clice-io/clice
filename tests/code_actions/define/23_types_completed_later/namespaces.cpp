namespace a {
struct C;
}

namespace b {
void §(other_namespace)f(a::C c);
}

namespace a {
struct C {};
}

struct D;

namespace {
D §(anonymous_namespace)f();
}

struct D {};

struct E;

E §(linkage_block)h();

extern "C" {
struct E {};
}

struct G;

namespace a {
G §(qualified)g();
}

struct G {};

struct F;

namespace a {
inline namespace v1 {
struct S {
    void §(inline_namespace)f(F f);
};
}
}

struct F {};
