// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Extra info for function call.
namespace fn_call {
void fun(int arg_a, int &arg_b) {};
void code() {
  int a = 1, b = 2;
  fun(a, §(01_arg_by_ref)b);
}
}

// make_unique-like function call.
namespace forwarded_arg {
struct Foo {
  explicit Foo(int arg_a) {}
};
template<class T, class... Args>
T make(Args&&... args)
{
    return T(args...);
}

void code() {
  int a = 1;
  auto foo = make<Foo>(§(02_forwarded_arg)a);
}
}

// Converted argument to const reference parameter.
namespace converted_arg {
void foobar(const float &arg);
int main() {
  int a = 0;
  foobar(§(03_converted_arg)a);
}
}

// Converted argument to explicit constructor.
namespace converted_ctor_arg {
struct Foo {
  explicit Foo(const float& arg) {}
};
int main() {
  int a = 0;
  Foo foo(§(04_converted_ctor_arg)a);
}
}

// Literal passed to function call.
namespace literal_arg {
void fun(int arg_a, const int &arg_b) {};
void code() {
  int a = 1;
  fun(a, §(05_literal_arg)2);
}
}

// Expression passed to function call.
namespace expression_arg {
void fun(int arg_a, const int &arg_b) {};
void code() {
  int a = 1;
  fun(a, 1 §(06_expression_arg)+ 2);
}
}

// Expression passed by value.
namespace expression_by_value {
int add(int lhs, int rhs);
int main() {
  add(1 §(07_expression_by_value)+ 2, 3);
}
}

// Literal converted to const reference parameter.
namespace converted_literal {
void foobar(const float &arg);
int main() {
  foobar(§(08_converted_literal)0);
}
}

// Extra info for method call.
namespace method_call {
class C {
 public:
  void fun(int arg_a = 3, int arg_b = 4) {}
};
void code() {
  int a = 1, b = 2;
  C c;
  c.fun(§(09_method_arg_default)a, b);
}
}

// Variable converted through a converting constructor.
namespace converting_ctor {
struct Foo {
  Foo(const int &);
};
void foo(Foo);
void bar() {
  const int x = 0;
  foo(§(10_converting_ctor_arg)x);
}
}
