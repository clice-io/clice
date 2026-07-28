// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

namespace fn_via_pointer {
// Function definition via pointer
void foo(int) {}
int main() {
  auto *X = &§(01_fn_via_pointer)foo;
}
}

namespace fn_via_call {
// Function declaration via call
int foo(int);
int main() {
  return §(02_fn_via_call)foo(42);
}
}

namespace fn_decl {
// Function declaration
void foo();
void g() { §(03_fn_decl)foo(); }
void foo() {}
}

// Function template with default argument instantiation.
namespace fn_default_tmpl_arg {
template <typename T = int>
void foo(const T& = T()) {
  §(04_fn_default_tmpl_arg)foo<>(3);
}
}
