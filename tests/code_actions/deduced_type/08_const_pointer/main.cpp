// A `const` before an `auto` that deduced a pointer moves behind the `*`,
// keeping the pointer itself constant; so does the `auto` of a decltype. When
// anything other than `static` stands between the `const` and the `auto`, a
// macro or a comment, the declaration is left as written. An `auto` copying
// another `auto` variable spells the type that one deduced. The
// static_asserts check the meaning after the edit.

#define CONST const
#define STORAGE static

int* make_pointer();
int* _Nonnull make_nonnull();
using Pointer = §(alias)decltype(make_pointer());

void f(int* pointer, int** table) {
    const §(pointer)auto p = pointer;
    static_assert(__is_same(decltype(p), int* const));
    const §(pointee)auto* row = table;
    static_assert(__is_same(decltype(row), int* const*));
    static const §(specifiers)auto s = pointer;
    static_assert(__is_same(decltype(s), int* const));
    const static §(parted)auto t = pointer;
    CONST §(macro)auto m = pointer;
    const /* owned */ §(comment)auto c = pointer;
    const STORAGE §(hidden)auto h = pointer;
    const §(nullability)auto n = make_nonnull();
    static_assert(__is_same(decltype(n), int* const));
    int* const fixed = pointer;
    const §(decltype_const)decltype(fixed) d = fixed;
    static_assert(__is_same(decltype(d), int* const));
}

auto copied = make_pointer();
const §(auto_source)auto copy = copied;
static_assert(__is_same(decltype(copy), int* const));
