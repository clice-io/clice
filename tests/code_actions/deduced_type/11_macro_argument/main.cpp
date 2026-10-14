// A declaration written in a macro argument is the argument's text: the
// edit replaces the `auto` written there. Nothing a macro body spells
// can be edited this way.

#define WRAP(declaration) declaration
#define DECLARE(name) auto name = 1;

int make();

void f() {
    WRAP(§(argument)auto a = make();)
    §(body)DECLARE(b)
    static_assert(__is_same(decltype(a), int));
}
