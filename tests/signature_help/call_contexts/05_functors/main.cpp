struct Adder {
    int operator()(int a, int b) const;
};

Adder make_adder();

int main() {
    Adder add;
    add(§(pos)1, 2);
    make_adder()(§(unnamed)1, 2);
}
