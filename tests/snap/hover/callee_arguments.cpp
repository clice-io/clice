// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Expression Context
///
/// ## Call arguments — which parameter each argument binds to
///
/// - status: supported
/// - order: 2
///
/// Hovering an argument at a call site shows the parameter it is passed to,
/// naming the parameter it binds.

namespace callee_arguments {

void configure(int width, int& out, int flags = 0);

void demo() {
  int w = 1024;
  int result = 0;
  configure(§(01_by_name)w, §(02_by_ref)result, §(03_literal)3);
}

}
