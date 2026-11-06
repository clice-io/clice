struct A {
    void §(first)foo();
};

struct B {
    void §(second)foo();
};

template <typename T>
void process(T& obj) {
    obj.foo();
}

void run(A a, B b) {
    process(a);
    process(b);
}
