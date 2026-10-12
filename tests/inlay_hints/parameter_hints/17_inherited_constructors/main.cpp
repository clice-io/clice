struct Base {
    Base(int width);
};

struct Derived : Base {
    using Base::Base;
};

Derived d(7);
