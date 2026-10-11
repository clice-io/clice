struct A {
    void §(member)foo();
};

template <typename T>
void process(T& obj) {
    obj.foo();
}

void run(A a) {
    process(a);
}
