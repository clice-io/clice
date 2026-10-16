struct Adder {
    int operator()(int left, int right) const;
};

int apply(int (*operation)(int, int), Adder adder, int value) {
    auto twice = [](int x) { return x * 2; };
    void (Adder::*member)() = nullptr;
    return operation(value, value) + adder(value, value) + twice(value);
}
