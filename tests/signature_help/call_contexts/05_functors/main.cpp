struct Adder {
    int operator()(int a, int b) const;
};

Adder make_adder();

struct Visitor {
    int operator()(int node);
    int operator()(int node) const;
};

int main() {
    Adder add;
    add(§(pos)1, 2);
    make_adder()(§(unnamed)1, 2);
    Visitor visit;
    visit(§(const_overloads)1);
}
