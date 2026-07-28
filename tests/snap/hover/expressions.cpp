// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// character literal
auto c = '§(01_char_literal)A';

// string literal
auto s = §(02_string_literal)"Hello, world!";

// sizeof expr
void sizeof_expr() {
  (void)size§(03_sizeof_expr)of(char);
}

// alignof expr
void alignof_expr() {
  (void)align§(04_alignof_expr)of(char);
}
