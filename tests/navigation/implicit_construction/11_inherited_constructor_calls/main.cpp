// Constructing a class through constructors it inherits with
// `using Base::Base` runs the base constructor: the parenthesis and the
// brace lead there, as they do for a constructor of the class itself.

struct Base {
    Base(int x);
};

struct Derived : Base {
    using Base::Base;
};

void build() {
    Derived d§(paren)(3);
    Derived e§(brace){4};
}
