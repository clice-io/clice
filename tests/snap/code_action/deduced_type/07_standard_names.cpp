/// # Standard names of builtin types
///
/// - status: supported
///
/// The types of `sizeof`, a pointer difference and `nullptr` expand to their standard names where those are declared
///
/// Without a declaration of `std::nullptr_t` in sight, the type of `nullptr` is written `decltype(nullptr)`.

void before() {
    §(null)auto null = nullptr;
}

namespace std {
using size_t = decltype(sizeof 0);
using ptrdiff_t = decltype((int*)0 - (int*)0);
using nullptr_t = decltype(nullptr);
}

void after(int* first, int* last) {
    §(size)auto size = sizeof(int);
    §(difference)auto difference = last - first;
    §(declared_null)auto null = nullptr;
}
