template <typename T>
void process(T& obj) {
    obj.§(template_call)foo();
}

struct A {
    void foo() {}
};

void run(A a) {
    process(a);
}

struct B {
    void bar() {}
};

void generic(B b) {
    auto call = [](auto& x) { x.§(lambda_call)bar(); };
    call(b);
}
