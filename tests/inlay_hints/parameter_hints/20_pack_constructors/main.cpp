// A pack passed on whole resolves to the callee's parameters, a
// constructor's included; inside `Foo{args...}` no hint lands.
struct Foo {
    Foo();
    Foo(int count);
};

void consume(Foo a, int b);

template <typename... Args>
void relay(Args... args) {
    consume(args...);
}

template <typename... Args>
void construct(Args... args) {
    // Inside Foo{args...} no hint lands: which constructor parameter a pack
    // element meets is only known per instantiation.
    consume(Foo{args...}, 1);
}

void use() {
    relay(Foo{}, 42);
    relay(42, 42);
    construct(42);
}
