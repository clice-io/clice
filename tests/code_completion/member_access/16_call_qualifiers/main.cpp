// A method shows its qualifiers after its parameters. A variable holding a
// lambda shows the parameters it is called with, without the `const` of the
// lambda's call operator.
struct Counter {
    int value() const;
    void reset();
};

void use(Counter counter) {
    auto twice = [](int x) { return x * 2; };
    counter.§(method);
    twi§(lambda);
}
