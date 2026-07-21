// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Typedef resolving through a chain of template aliases.
namespace typedef_chain {
template <bool X, typename T, typename F>
struct cond { using type = T; };
template <typename T, typename F>
struct cond<false, T, F> { using type = F; };

template <bool X, typename T, typename F>
using type = typename cond<X, T, F>::type;

void foo() {
  using f§(02_typedef_chain)oo = type<true, int, double>;
}
}

struct FwdFoo;
int fwd_bar;
auto fwd_baz = (Fwd§(01_forward_struct_value)Foo*)&fwd_bar;

#define A(x) x, x, x, x
#define B(x) A(A(A(A(x))))
int a§(03_big_initializer)rr[] = {B(0)};
