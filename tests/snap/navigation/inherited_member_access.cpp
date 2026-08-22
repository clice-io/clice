/// - verify: server

struct §(base_def)Base {
    int §(method_def)measure() const {
        return 5;
    }
};

struct §(derived_def)Derived : Base {};

int measure_derived(const Derived& derived) {
    return derived.§(inherited_use)measure();
}
