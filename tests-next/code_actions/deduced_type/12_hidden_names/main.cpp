// A type name hidden where the declaration stands, by a local variable, by a
// namespace a using-directive brings in through another, inside a template
// argument next to a character literal, or with a name outside ASCII, is
// written from the global scope; a type a variable of the same name hides
// everywhere stays auto. The static_asserts check the meaning after the
// edit.

struct X {};
X make_x();

void local() {
    int X = 0;
    §(local)auto v = make_x();
    static_assert(__is_same(decltype(v), ::X));
}

namespace shadow {
struct a {};
}  // namespace shadow
namespace mid {
using namespace shadow;
}  // namespace mid
namespace a {
struct T {};
}  // namespace a
a::T make_t();

namespace n {
using namespace mid;
void f() {
    §(transitive)auto t = make_t();
    static_assert(__is_same(decltype(t), ::a::T));
}
}  // namespace n

struct Y {};
Y make_y();
int Y;

void g() {
    §(variable)auto y = make_y();
}

template <class A, class B>
struct Pair {};
template <char C>
struct Tag {};
Pair<X, Tag<'X'>> make_pair();

namespace literal {
struct X {};
void f() {
    §(literal)auto v = make_pair();
    static_assert(__is_same(decltype(v), Pair<::X, Tag<'X'>>));
}
}  // namespace literal

struct Å {};
Å make_å();

namespace u {
struct Å {};
void f() {
    §(unicode)auto v = make_å();
    static_assert(__is_same(decltype(v), ::Å));
}
}  // namespace u
