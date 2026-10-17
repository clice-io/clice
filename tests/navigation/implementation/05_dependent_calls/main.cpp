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

template <typename T>
void twice(T& obj) {
    obj.§(several_calls)foo();
}

struct C {
    void foo() {}
};

void both(A a, C c) {
    twice(a);
    twice(c);
}

template <typename T>
int convert(T& value) {
    return value.§(conversion_call)operator int();
}

struct D {
    operator int() {
        return 4;
    }
};

int use_convert(D d) {
    return convert(d);
}
