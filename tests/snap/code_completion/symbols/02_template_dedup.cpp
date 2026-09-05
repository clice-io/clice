/// # Class template deduplication
///
/// - status: supported
/// - diagnostics: expected
///
/// A name that is also constructors and a deduction guide stays a single class entry

// The completion prefix dangles as an unfinished statement.
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
