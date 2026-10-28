template <typename T>
struct Foo {
    Foo() {}

    Foo(T x) {}

    Foo(T x, T y) {}
};

template <typename T>
Foo(T) -> Foo<T>;

void bar() {
    Fo§(pos);
}
