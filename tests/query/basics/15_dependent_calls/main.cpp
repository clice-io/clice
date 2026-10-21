struct A {
    void foo() {}
};
struct B {
    void bar() {}
};

template <class T>
void process(T& obj) {
    obj.foo();
}

void run() {
    A a;
    process(a);
    auto generic = [](auto& x) { x.bar(); };
    B b;
    generic(b);
}
