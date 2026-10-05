/// # Names spelled for the scope
///
/// - status: supported
///
/// A name drops the enclosing namespaces only as far as the shorter name still finds the same type
///
/// A name hidden by a declaration closer to the expansion keeps its qualifier, and one hidden even when fully qualified starts from the global scope.

struct Global {};
Global make_global();

namespace outer {
template <typename T>
struct Box {};
struct Item {};
Box<Item> make_box();

namespace inner {
struct Item {};
struct Global {};

void f() {
    §(argument)auto box = make_box();
    §(global)auto global = ::make_global();
}
}  // namespace inner
}  // namespace outer
