// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

namespace std
{
  template<class _E>
  class initializer_list { const _E *a, *b; };
}

// auto on structured bindings.
namespace structured_bindings {
void foo() {
  struct S { int x; float y; };
  au§(01_structured_bindings)to [x, y] = S();
}
}

// Undeduced auto.
namespace undeduced {
template<typename T>
void foo() {
  au§(02_undeduced_auto)to x = T{};
}
}

// Constrained auto.
namespace constrained {
template <class T> concept F = true;
F au§(03_constrained_auto)to x = 1;
}

// auto on lambda.
namespace on_lambda {
void foo() {
  au§(04_auto_lambda)to lamb = []{};
}
}

// auto on template instantiation.
namespace on_instantiation {
template<typename T> class Foo{};
void foo() {
  au§(05_auto_instantiation)to x = Foo<int>();
}
}

// auto on specialized template.
namespace on_specialization {
template<typename T> class Foo{};
template<> class Foo<int>{};
void foo() {
  au§(06_auto_specialized)to x = Foo<int>();
}
}

// Simple initialization with auto.
namespace simple_init {
void foo() {
  §(07_simple_auto)auto i = 1;
}
}

// Simple initialization with const auto.
namespace const_init {
void foo() {
  const §(08_const_auto)auto i = 1;
}
}

// Simple initialization with const auto&.
namespace const_ref_init {
void foo() {
  const §(09_const_auto_ref)auto& i = 1;
}
}

// Simple initialization with auto&.
namespace ref_init {
void foo() {
  int x;
  §(10_auto_ref)auto& i = x;
}
}

// Simple initialization with auto*.
namespace ptr_init {
void foo() {
  int a = 1;
  §(11_auto_ptr)auto* i = &a;
}
}

// Simple initialization with auto from pointer.
namespace from_pointer {
void foo() {
  int a = 1;
  §(12_auto_from_pointer)auto i = &a;
}
}

// Auto with initializer list.
namespace init_list {
void foo() {
  §(13_auto_init_list)auto i = {1,2};
}
}

// User defined conversion to auto.
namespace conversion {
struct Bar {
  operator §(14_auto_conversion)auto() const { return 10; }
};
}

// Simple trailing return type.
namespace trailing_return {
§(15_auto_trailing_return)auto main() -> int {
  return 0;
}
}

namespace trailing_decltype_return {
// auto function return with trailing type
struct Bar {};
§(16_auto_trailing_decltype)auto test() -> decltype(Bar()) {
  return Bar();
}
}

namespace fn_return {
// auto in function return
struct Bar {};
§(17_auto_fn_return)auto test() {
  return Bar();
}
}

namespace ref_fn_return {
// auto& in function return
struct Bar {};
§(18_auto_ref_fn_return)auto& test() {
  static Bar x;
  return x;
}
}

namespace ptr_fn_return {
// auto* in function return
struct Bar {};
§(19_auto_ptr_fn_return)auto* test() {
  Bar* bar;
  return bar;
}
}

namespace const_ref_fn_return {
// const auto& in function return
struct Bar {};
const §(20_const_auto_ref_fn_return)auto& test() {
  static Bar x;
  return x;
}
}

// More complicated structured types.
namespace fn_pointer {
int bar();
§(21_auto_fn_pointer)auto (*foo)() = bar;
}

// auto on alias.
namespace alias_int {
typedef int int_type;
§(22_auto_alias_int)auto x = int_type();
}

namespace alias_class {
// auto on alias
struct cls {};
typedef cls cls_type;
§(23_auto_alias_class)auto y = cls_type();
}

namespace alias_template_inst {
// auto on alias
template <class>
struct templ {};
§(24_auto_alias_template)auto z = templ<int>();
}

// Undeduced auto declaration.
namespace undeduced_decl {
template<typename T>
void foo() {
  §(25_undeduced_auto_decl)auto x = T();
}
}

// Undeduced auto return type.
namespace undeduced_return {
template<typename T>
§(26_undeduced_auto_return)auto foo() {
  return T();
}
}

// Template auto parameter.
namespace template_auto_param {
template<a§(27_template_auto_param)uto T>
  void func() {
}
}
