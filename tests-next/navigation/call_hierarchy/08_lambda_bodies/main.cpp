void §(foo)foo() {}

void use() {
    auto task = [] {
        foo();
    };
    task();
}
