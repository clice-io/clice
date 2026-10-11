// The completion prefixes dangle as unfinished statements.
int foooo(int x, float y);
void nothing_to_fill();

struct Foo {
    int bazzzz(int a, int b);
};

void bar() {
    Foo f;
    fo§(free_function);
    no§(no_arguments);
    f.ba§(method);
}
