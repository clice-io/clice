// A variable declared with a decltype type, reached through a qualified
// name or a using-directive: expand auto writes the type the decltype
// denotes, the one inside a pointer stays as declared.

namespace n {
struct M {};
decltype(M{}) m;
decltype(M{})* pm;
}

void f() {
    §(qualified)auto x = n::m;
    §(pointer)auto y = n::pm;
}

using namespace n;

void g() {
    §(directive)auto z = m;
}
