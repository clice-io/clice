// Implementation on a virtual function lists the overrides at every level
// below it, a pure one in between included.

struct A {
    virtual void §(pure_root)f() = 0;
};

struct B : A {
    void f() override = 0;
};

struct C : B {
    void f() override {}
};

struct P {
    virtual void §(concrete_root)g() = 0;
};

struct Q : P {
    void g() override {}
};

struct R : Q {
    void g() override {}
};
