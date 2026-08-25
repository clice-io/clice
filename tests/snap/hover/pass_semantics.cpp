// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Expression Context
///
/// ## Pass semantics — by value, by reference, by const reference
///
/// - status: supported
/// - order: 3
///
/// The argument card states how the value reaches the callee: copied by
/// value, or bound to a mutable or const reference parameter.

namespace pass_semantics {

void by_value(int x);
void by_ref(int& x);
void by_const_ref(const int& x);

void demo() {
  int n = 0;
  by_value(§(01_value)n);
  by_ref(§(02_ref)n);
  by_const_ref(§(03_const_ref)n);
}

}
