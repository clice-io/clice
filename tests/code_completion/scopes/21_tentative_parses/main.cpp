// Completion in code clang parses tentatively first, as an assertion macro's
// condition declaration, offers what the same code offers elsewhere: the
// names the included headers declare among them.
#include "lib.h"

struct Result {
    explicit operator bool() const;
};

Result compare(int, int);

#define CHECK_EQ(a, b)                                                                             \
    if(const Result result = compare((a), (b)))                                                    \
        ;                                                                                          \
    else

void check(lib::Value v) {
    CHECK_EQ(v.§(macro_condition), 1);
}

template <class T>
concept nested = requires { typename lib::§(requires_typename); };

template <class T>
concept global = requires { typename ::§(requires_global); };

void plain() {
    lib::§(namespace_scope);
}
