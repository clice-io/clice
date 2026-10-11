struct Base {
    int base_field;
    int base_method();
};

struct Derived : Base {
    int derived_field;
};

void bar() {
    Derived d;
    d.§(pos);
}
