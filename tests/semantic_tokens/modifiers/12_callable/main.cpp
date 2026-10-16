struct Adder {
    int operator()(int left, int right) const;
};

struct Counter : Adder {};

int apply(int (*operation)(int, int), Adder adder, Counter counter, int value) {
    auto twice = [](int x) { return x * 2; };
    void (Adder::*member)() = nullptr;
    return operation(value, value) + adder(value, value) + counter(value, 1) + twice(value);
}
