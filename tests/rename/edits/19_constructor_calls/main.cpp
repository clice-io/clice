struct §(base)Base {
    Base(int value);
};

struct Derived : Base {
    using Base::Base;
};

using Alias = Base;

void build() {
    auto a = Base(1);
    auto b = Alias(2);
    auto c = Derived(3);
    auto* d = new §(call)Base(4);
}
