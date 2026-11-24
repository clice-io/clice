struct §(base)Base {
    Base(int value);
};

struct Derived : Base {
    using Base::Base;
};

using Alias = Base;

namespace v2 {
using Base = ::Base;
}

template <typename T>
void construct(T value) {
    auto e = v2::§(alias_template)Base(value);
}

struct Traits {
    using Base = ::Base;
};

template <typename T>
auto construct_member() {
    return typename T::§(dependent)Base(6);
}

void build() {
    auto a = Base(1);
    auto b = §(alias_call)Alias(2);
    auto c = §(derived_call)Derived(3);
    auto* d = new §(call)Base(4);
    construct(5);
    construct_member<Traits>();
}
