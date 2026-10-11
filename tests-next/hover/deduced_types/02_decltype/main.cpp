namespace declared_types {
long source = 0;

void locals() {
    long value = 1;
    const long constant = 2;
    long& reference = value;
    §(value)decltype(auto) first = 3L;
    §(const)decltype(auto) second = constant;
    §(ref)decltype(auto) third = reference;
    §(of_lvalue)decltype(value) fourth = value;
    §(of_paren)decltype((value)) fifth = value;
    §(of_rvalue)decltype(static_cast<long&&>(value)) sixth = static_cast<long&&>(value);
}

decltype(source) §(var_type)copy = source;

template <typename T> §(undeduced)decltype(auto) pending() {
    return T{};
}

template <typename T> struct Member {
    using type = §(dependent)decltype(T::value);
};
}
