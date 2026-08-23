// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Documentation
///
/// ## Doxygen `///` comments — extracted from the declaration and rendered on hover
///
/// - status: supported
/// - order: 1
///
/// Applies to plain functions, primary templates and their specializations;
/// a reference resolves to the most specialized declaration's comment.

namespace docs {
/// Adds two integers.
int §(01_function)add(int a, int b);

/// A box holding a value.
template <typename T> struct §(02_primary_def)Box {};

/// A box of pointers.
template <typename T> struct §(03_spec_def)Box<T*> {};

void use() {
    Box§(04_primary_ref)<int> b;
    Box§(05_spec_ref)<int*> p;
}
}
