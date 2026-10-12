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
