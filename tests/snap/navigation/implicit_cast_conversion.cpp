/// # Implicit Code Navigation
///
/// ## Casts invoking a constructor or conversion operator
///
/// - status: partial
/// - verify: server
/// - order: 18
///
/// A `static_cast` that constructs its target reaches the selected
/// constructor. A `static_cast` that runs a user-defined conversion operator
/// does not yet reach the operator.

struct Meters {
    explicit operator double() const;
};

struct Foo {
    explicit Foo(int value);
};

void use(Meters m) {
    double d = §(cast_conv)static_cast<double>(m);
    Foo f = §(cast_ctor)static_cast<Foo>(42);
}
