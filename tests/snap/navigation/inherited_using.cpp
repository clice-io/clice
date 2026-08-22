/// - verify: server

struct Base {
    §(base_ctor)Base(int input) : value(input) {}

    int §(base_member)read() const {
        return value;
    }

    int value;
};

struct Derived : Base {
    using Base::§(ctor_using)Base;
    using Base::§(member_using)read;
};

int read_derived() {
    §(derived_use)Derived derived(3);
    return derived.§(member_use)read();
}
