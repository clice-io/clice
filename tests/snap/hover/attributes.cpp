// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Special Hover Targets
///
/// ## Attribute documentation — hovering an attribute shows its description
///
/// - status: supported
/// - order: 4
/// - issues: clangd#1862
///
/// The attribute's own documentation renders in the card, for both GNU
/// `__attribute__` spellings and C++ `[[...]]` attributes.

namespace attr_docs {
void foo(int * __attribute__((non§(gnu_attribute)null, noescape)) );

[[nodi§(std_attribute)scard]] int compute();
}
