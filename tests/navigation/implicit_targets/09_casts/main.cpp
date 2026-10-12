struct Meters {
    explicit operator double() const;
};

struct Foo {
    explicit Foo(int value);
};

void use(Meters m) {
    double d = §(cast_conv)static_cast<double>(m);
    double e = §(c_cast)(double)m;
    Foo f = §(cast_ctor)static_cast<Foo>(42);
}
