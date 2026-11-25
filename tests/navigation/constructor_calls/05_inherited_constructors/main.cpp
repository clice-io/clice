struct Base {
    Base(int value);
};

struct Derived : Base {
    using Base::Base;
};

void build() {
    auto a = §(cast)Derived(1);
    auto* b = new §(allocated)Derived(2);
}
