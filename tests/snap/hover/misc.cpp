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

// Labels.
namespace labels {
inline int f(int x) {
  if (x) goto §(04_goto_label)done;
  x += 1;
§(05_label_def)done:
  return x;
}
}

// Class-provided allocation functions.
namespace class_new_delete {
struct Pool {
  static void *operator new(unsigned long n);
  static void operator delete(void *p);
};
Pool *p = §(06_operator_new)new Pool;
inline void f() { §(07_operator_delete)delete p; }
}

// Globally qualified allocation and construction punctuation.
namespace global_alloc {
struct Pool {
  static void *operator new(unsigned long n);
  Pool(int);
};
Pool *p = ::§(08_global_new)new Pool(0);
Pool value§(09_ctor_paren)(1);
}

// Overloaded call and subscript punctuation.
namespace op_punct {
struct F {
  int operator()(int);
  int operator[](int);
};
int u(F f) { return f§(10_op_call)(1) + f§(11_op_subscript)[2]; }
}
