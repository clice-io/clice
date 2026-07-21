// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Pointers to lambdas.
namespace ptr_to_lambda {
void foo() {
  auto lamb = [](int T, bool B) -> bool { return T && B; };
  auto *b = &lamb;
  auto *§(01_lambda_ptr_ptr)c = &b;
}
}

// Lambda parameter with decltype reference.
namespace decltype_ref_param {
auto lamb = [](int T, bool B) -> bool { return T && B; };
void foo(decltype(lamb)& bar) {
  ba§(02_lambda_decltype_ref_param)r(0, false);
}
}

// Lambda parameter with decltype.
namespace decltype_param {
auto lamb = [](int T, bool B) -> bool { return T && B; };
void foo(decltype(lamb) bar) {
  ba§(03_lambda_decltype_param)r(0, false);
}
}

// Lambda variable.
namespace lambda_variable {
void foo() {
  int bar = 5;
  auto lamb = [&bar](int T, bool B) -> bool { return T && B && bar; };
  bool res = lam§(04_lambda_variable)b(bar, false);
}
}

// Local variable in lambda.
namespace local_in_lambda {
void foo() {
  auto lamb = []{int te§(05_local_in_lambda)st;};
}
}
