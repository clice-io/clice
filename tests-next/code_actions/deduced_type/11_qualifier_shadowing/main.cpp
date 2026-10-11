// A type keeps the qualifiers a closer declaration of its name requires,
// inside a template argument too, and a name hidden even when qualified
// starts from the global scope. The static_asserts check the meaning after
// the edit.

namespace a {
template <class T>
struct Box {};
struct X {};
X make();
Box<X> make_box();
namespace b {
struct X {};
void f() {
    §(inner)auto x = a::make();
    static_assert(__is_same(decltype(x), a::X));
    §(argument)auto box = a::make_box();
    static_assert(__is_same(decltype(box), a::Box<a::X>));
}
}  // namespace b
}  // namespace a

struct G {};
G make_global();

namespace ns {
struct G {};
struct T {};
T make_t();
void f() {
    §(global)auto g = ::make_global();
    static_assert(__is_same(decltype(g), ::G));
}
template <class T>
void g() {
    §(parameter)auto t = make_t();
    static_assert(__is_same(decltype(t), ns::T));
}
template void g<int>();
}  // namespace ns
