/// # Go to Implementation
///
/// ## Template duck-type navigation
///
/// - status: unsupported
/// - order: 5
///
/// From a dependent member call, go-to-implementation should list the
/// concrete methods of every known instantiation; the same applies to a
/// generic lambda's dependent calls. Today it returns nothing.

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
