// Macros as everyday code writes them expand as the preprocessor does:
// stringizing, token pasting, variadic arguments, a statement wrapped in
// do-while, an argument that is itself a macro, and a macro an included
// header defines.

#include "config.h"

#define STR(x) #x
#define CAT(a, b) a##b
#define CALL(f, ...) f(__VA_ARGS__)
#define SWAP(a, b) do { int t = a; a = b; b = t; } while (0)
#define SQUARE(x) ((x) * (x))
#define LIMIT 4

int foobar = 1;
int add(int x, int y);

void f(int a, int b) {
    const char* s = §(stringize)STR(hello);
    int c = §(paste)CAT(foo, bar);
    int d = §(variadic)CALL(add, a, b);
    §(statement)SWAP(a, b);
    int e = §(macro_argument)SQUARE(LIMIT);
    int m = §(from_header)MAX_ITEMS;
}
