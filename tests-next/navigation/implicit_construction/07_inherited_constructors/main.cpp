struct Base {
    Base(int x);
    Base(int x, int y);
};

struct Derived : Base {
    using Base::§(inherit)Base;
};
