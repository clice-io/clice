/// # Symbols
///
/// ## Class template deduplication — a name that is also constructors and a deduction guide stays a single class entry
///
/// - status: supported
/// - order: 2

// error-ok: the completion prefix dangles as an unfinished statement.
template <typename T>
struct Foo {
    Foo() {}

    Foo(T x) {}

    Foo(T x, T y) {}
};

template <typename T>
Foo(T) -> Foo<T>;

void bar() {
    Fo§(pos)
}
