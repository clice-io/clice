// A name is checked where the declaration stands: a using-directive in the
// function, a friend function defined in a class with a member of that name,
// and a member defined out of line whose template parameter shares the name
// all keep the qualifier. The static_asserts check the meaning after the
// edit.

namespace app {
namespace v2 {
struct Config {};
}  // namespace v2
struct Config {};
Config load();

void directive() {
    using namespace v2;
    §(directive)auto c = load();
    static_assert(__is_same(decltype(c), app::Config));
}

struct Node {};
Node make_node();

struct Tree {
    struct Node {};

    friend void visit(Tree&) {
        §(friend)auto n = make_node();
        static_assert(__is_same(decltype(n), app::Node));
    }
};

struct U {};
U make_u();

template <class T>
struct S {
    void f();
};

template <class U>
void S<U>::f() {
    §(outer)auto u = make_u();
    static_assert(__is_same(decltype(u), app::U));
}

template struct S<int>;
}  // namespace app
