// The types of `sizeof`, a pointer difference and `nullptr` take their standard
// names once those are declared; before, `sizeof` stays auto and `nullptr` is
// written `decltype(nullptr)`. A global variable after the declarations is
// asked too. The static_asserts check the meaning after the edit.

void f() {
    §(size)auto n = sizeof(int);
    §(null)auto p = nullptr;
    static_assert(__is_same(decltype(p), decltype(nullptr)));
}

namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int*)0 - (int*)0);
using nullptr_t = decltype(nullptr);
}  // namespace std

§(global)auto global_size = sizeof(int);

void g(int* a, int* b) {
    §(size_std)auto n = sizeof(int);
    static_assert(__is_same(decltype(n), std::size_t));
    §(difference)auto d = b - a;
    static_assert(__is_same(decltype(d), std::ptrdiff_t));
    §(null_std)auto p = nullptr;
    static_assert(__is_same(decltype(p), std::nullptr_t));
}
