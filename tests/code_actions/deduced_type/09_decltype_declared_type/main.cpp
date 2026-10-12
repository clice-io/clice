// A variable declared with a decltype type, reached through a qualified
// name or a using-directive: expand auto writes the type the decltype
// denotes, the one inside a pointer stays as declared. The static_asserts
// check the meaning after the edit.

namespace n {
struct M {};
decltype(M{}) m;
decltype(M{})* pm;
}

void f() {
    §(qualified)auto x = n::m;
    static_assert(__is_same(decltype(x), n::M));
    §(pointer)auto y = n::pm;
    static_assert(__is_same(decltype(y), n::M*));
}

using namespace n;

void g() {
    §(directive)auto z = m;
    static_assert(__is_same(decltype(z), n::M));
}
