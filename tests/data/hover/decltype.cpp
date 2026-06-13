// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Simple initialization with decltype(auto).
namespace simple_init {
void foo() {
  decl$(01_decltype_auto)type(auto) i = 1;
}
}

// Simple initialization with const decltype(auto).
namespace const_init {
void foo() {
  const int j = 0;
  decl$(02_const_decltype_auto)type(auto) i = j;
}
}

// Simple initialization with const& decltype(auto).
namespace const_ref_init {
void foo() {
  int k = 0;
  const int& j = k;
  decl$(03_const_ref_decltype_auto)type(auto) i = j;
}
}

// Simple initialization with & decltype(auto).
namespace ref_init {
void foo() {
  int k = 0;
  int& j = k;
  decl$(04_ref_decltype_auto)type(auto) i = j;
}
}

namespace fn_return {
// decltype(auto) in function return
struct Bar {};
decl$(05_decltype_auto_fn_return)type(auto) test() {
  return Bar();
}
}

// decltype(auto) reference in function return.
namespace ref_fn_return {
decl$(06_decltype_auto_ref_return)type(auto) test() {
  static int a;
  return (a);
}
}

// decltype of lvalue.
namespace of_lvalue {
void foo() {
  int I = 0;
  decl$(07_decltype_lvalue)type(I) J = I;
}
}

// decltype of lvalue reference.
namespace of_lvalue_ref {
void foo() {
  int I = 0;
  int &K = I;
  decl$(08_decltype_lvalue_ref)type(K) J = I;
}
}

// decltype of parenthesized lvalue.
namespace of_paren_lvalue {
void foo() {
  int I = 0;
  decl$(09_decltype_paren_lvalue)type((I)) J = I;
}
}

// decltype of rvalue reference.
namespace of_rvalue_ref {
void foo() {
  int I = 0;
  decl$(10_decltype_rvalue_ref)type(static_cast<int&&>(I)) J = static_cast<int&&>(I);
}
}

// decltype of rvalue reference function call.
namespace of_rvalue_call {
int && bar();
void foo() {
  int I = 0;
  decl$(11_decltype_rvalue_call)type(bar()) J = bar();
}
}

namespace of_trailing_return_fn {
// decltype of function with trailing return type.
struct Bar {};
auto test() -> decltype(Bar()) {
  return Bar();
}
void foo() {
  decl$(12_decltype_trailing_return)type(test()) i = test();
}
}

// decltype of var with decltype.
namespace of_decltype_var {
void foo() {
  int I = 0;
  decltype(I) J = I;
  decl$(13_decltype_of_decltype)type(J) K = J;
}
}

// decltype of dependent type.
namespace of_dependent {
template <typename T>
struct X {
  using Y = decl$(14_decltype_dependent)type(T::Z);
};
}

// Undeduced decltype(auto) return type.
namespace undeduced_return {
template<typename T>
decl$(15_undeduced_decltype_auto)type(auto) foo() {
  return T();
}
}

// Variable whose type is written with decltype.
namespace var_with_decltype {
int a;
decltype(a) $(16_var_decltype_type)b = a;
}

// Variable whose type chains through decltype.
namespace var_with_decltype_chain {
int a;
decltype(a) c;
decltype(c) $(17_var_decltype_chain)b = a;
}

// Variable with const decltype type.
namespace var_with_const_decltype {
int a;
const decltype(a) $(18_var_const_decltype)b = a;
}

// Function with decltype in the signature.
namespace fn_with_decltype {
int a;
auto $(19_fn_decltype_signature)foo(decltype(a) x) -> decltype(a) { return 0; }
}
