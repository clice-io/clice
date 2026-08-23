// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Type Information
///
/// ## `decltype` deduction — value, reference and dependent forms
///
/// - status: supported
/// - order: 6
///
/// Hovering a `decltype` or `decltype(auto)` placeholder shows the resolved
/// type, including the reference the parenthesized-expression rule adds.

namespace decltype_deduction {

int base = 0;

void locals() {
  int n = 0;
  const int cn = 0;
  int& r = n;
  §(01_value)decltype(auto) a = 1;
  §(02_const)decltype(auto) b = cn;
  §(03_ref)decltype(auto) c = r;
  §(04_of_lvalue)decltype(n) d = n;
  §(05_of_paren)decltype((n)) e = n;
  §(06_of_rvalue)decltype(static_cast<int&&>(n)) f = static_cast<int&&>(n);
}

decltype(base) §(07_var_type)mirror = base;

template <typename T> §(08_undeduced)decltype(auto) undeduced() { return T(); }

template <typename T> struct Dependent {
  using kind = §(09_dependent)decltype(T::member);
};

}
