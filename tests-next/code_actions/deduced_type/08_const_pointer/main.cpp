// A `const` before an `auto` that deduced a pointer moves behind the `*`,
// keeping the pointer itself constant; so does the `auto` of a decltype. When
// anything other than `static` stands between the `const` and the `auto`, a
// macro or a comment, the declaration is left as written. The static_asserts
// check the meaning after the edit.

#define CONST const
#define STORAGE static

int* make_pointer();
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
}
