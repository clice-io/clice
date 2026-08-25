// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Type Information
///
/// ## `auto` deduction — the type the placeholder resolves to
///
/// - status: supported
/// - order: 5
///
/// Hovering an `auto` placeholder shows the type substituted for it —
/// builtins, pointers, lambdas, template instantiations, and the
/// `/* not deduced */` marker inside an uninstantiated template.

namespace auto_deduction {

struct Bar {};
struct Pair { int first; int second; };
template <typename T> struct Box {};

void locals() {
  int n = 0;
  §(01_simple)auto a = 1;
  const §(02_const)auto b = 1;
  §(03_ref)auto& c = n;
  §(04_ptr)auto* d = &n;
  §(05_from_pointer)auto e = &n;
  §(06_lambda)auto f = []{};
  §(07_instantiation)auto g = Box<int>();
  §(08_structured)auto [x, y] = Pair{};
}

§(09_trailing_return)auto with_trailing() -> int { return 0; }

§(10_fn_return)auto deduced_return() { return Bar(); }

template <typename T> void undeduced() {
  §(11_undeduced)auto u = T();
}

}
