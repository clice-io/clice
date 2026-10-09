/// # Template duck-type navigation
///
/// - status: unsupported
///
/// Dependent calls do not resolve to methods of known instantiations yet
///
/// This applies to function templates and generic lambdas, but neither
/// currently returns an implementation target.

template <typename T>
void process(T& obj) {
    obj.foo();  // go-to-impl on foo → A::foo (from the process(a) instantiation)
}

struct A {
    void foo() {}
};

void run(A a) {
    process(a);
}

void generic() {
    auto call = [](auto& x) { x.bar(); };  // go-to-impl on bar → the concrete bar
}
