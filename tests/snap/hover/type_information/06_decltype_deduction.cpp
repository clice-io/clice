/// # `decltype` deduction
///
/// - status: supported
///
/// Decltype hover distinguishes value, reference and dependent forms
///
/// Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
/// type, including the reference the parenthesized-expression rule adds.

namespace declared_types {
long source = 0;

void locals() {
    long value = 1;
    const long constant = 2;
    long& reference = value;
    §(01_value)decltype(auto) first = 3L;
    §(02_const)decltype(auto) second = constant;
    §(03_ref)decltype(auto) third = reference;
    §(04_of_lvalue)decltype(value) fourth = value;
    §(05_of_paren)decltype((value)) fifth = value;
    §(06_of_rvalue)decltype(static_cast<long&&>(value)) sixth = static_cast<long&&>(value);
}

decltype(source) §(07_var_type)copy = source;

template <typename T> §(08_undeduced)decltype(auto) pending() {
    return T{};
}

template <typename T> struct Member {
    using type = §(09_dependent)decltype(T::value);
};
}
