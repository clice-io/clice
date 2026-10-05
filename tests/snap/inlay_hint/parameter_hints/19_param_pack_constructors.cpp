/// # Packs in constructor arguments
///
/// - status: partial
///
/// Outer calls resolve; hints inside the expansion are still missing

struct Foo {
    Foo();
    Foo(int x);
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
