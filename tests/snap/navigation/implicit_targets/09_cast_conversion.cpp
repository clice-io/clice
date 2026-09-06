/// # Cast conversion navigation
///
/// - status: partial
/// - verify: server
///
/// Constructing casts navigate to the selected constructor
///
/// A `static_cast` that runs a user-defined conversion operator does not yet
/// reach that operator.

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
