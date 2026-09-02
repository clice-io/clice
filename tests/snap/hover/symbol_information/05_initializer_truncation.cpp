// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Initializer truncation — huge initializers render truncated, not in full
///
/// - status: partial
/// - issues: clangd#710
///
/// The rendered definition omits the initializer, but the evaluated
/// `Value` field still spells out all 256 elements.

#define A(x) x, x, x, x
#define B(x) A(A(A(A(x))))
int a§(big_initializer)rr[] = {B(0)};
