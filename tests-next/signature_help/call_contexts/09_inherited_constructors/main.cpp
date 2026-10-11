struct Base {
    Base(int width, int height);
};

struct Derived : Base {
    using Base::Base;
};

Derived derived(§(inherited)1, 2);
