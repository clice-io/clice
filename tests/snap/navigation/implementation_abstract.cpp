// - verify: server
//
// An override that is itself pure implements nothing: implementation on the
// root goes through it to the concrete override below. A concrete
// intermediate stops the walk, its own overriders belong to it.

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
