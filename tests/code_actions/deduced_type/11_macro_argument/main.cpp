// Code a macro takes part in is left as written, a declaration in a macro
// argument too: the argument may be expanded more than once, a comma the
// edit brings splits it, and the macro's own tokens around it change what
// the edit means.

#define WRAP(declaration) declaration
#define DECLARE(name) auto name = 1;

int make();

void f() {
    WRAP(§(argument)auto a = make();)
    §(body)DECLARE(b)
    static_assert(__is_same(decltype(a), int));
}
