// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.
// error-ok: should not crash on invalid semantic form of init-list-expr,
// and the point is expected to produce NO hover.

struct Foo {
  int xyz = 0;
};
class Bar {};
constexpr Foo s = $(01_invalid_init_list){
  .xyz = Bar(),
};
