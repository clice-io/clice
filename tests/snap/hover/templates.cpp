// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Variable with template type.
namespace var_tmpl_type {
template <typename T, class... Ts> class Foo { public: Foo(int); };
Foo<int, char, bool> fo§(01_var_of_template_type)o = Foo<int, char, bool>(5);
}

// Implicit template instantiation.
namespace implicit_instantiation {
template <typename T> class vector{};
vec§(02_implicit_instantiation)tor<int> foo;
}

// Class template.
namespace class_template {
template <template<typename, bool...> class C,
          typename = char,
          int = 0,
          bool Q = false,
          class... Ts> class Foo final {};
template <template<typename, bool...> class T>
F§(03_class_template)oo<T> foo;
}

// Function template.
namespace function_template {
template <template<typename, bool...> class C,
          typename = char,
          int = 0,
          bool Q = false,
          class... Ts> void foo();
template<typename, bool...> class Foo;

void bar() {
  fo§(04_function_template)o<Foo>();
}
}

// Function decl.
namespace function_decl {
template<typename, bool...> class Foo {};
Foo<bool, true, false> foo(int, bool T = false);

void bar() {
  fo§(05_function_decl)o(3);
}
}

// Partially-specialized class template.
namespace partial_spec {
template <typename T> class X;
template <typename T> class §(06_partial_specialization)X<T*> {};
}

// Constructor of partially-specialized class template.
namespace partial_spec_ctor {
template<typename, typename=void> struct X;
template<typename T> struct X<T*>{ §(07_partial_spec_constructor)X(); };
}

namespace destructor {
class X { §(08_destructor)~X(); };
}

namespace conversion_operator {
class X { op§(09_conversion_operator)erator int(); };
}

namespace conversion_target {
class X { operator §(10_conversion_target)X(); };
}

// An uninstantiated specialization documents itself with the pattern
// instantiation would pick, not the primary template.
namespace primary_fallback {
// comment from primary
template <typename T> class Foo {};
// comment from specialization
template <typename T> class Foo<T*> {};
void foo() {
  Fo§(11_primary_template_doc)o<int*> *x = nullptr;
}
}

// Var template decl.
namespace var_template {
using m_int = int;

template <int Size> m_int §(12_variable_template)arr[Size];
}

// Var template decl specialization.
namespace var_template_spec {
using m_int = int;

template <int Size> m_int arr[Size];

template <> m_int §(13_variable_template_spec)arr<4>[4];
}

// Canonical type.
namespace canonical_type {
template<typename T>
struct TestHover {
  using Type = T;
};

void code() {
  TestHover<int>::Type §(14_canonical_type)a;
}
}

// Canonical template type.
namespace canonical_tmpl_type {
template<typename T>
void §(15_function_template_type)foo(T arg) {}
}

// TypeAlias template.
namespace alias_template {
template<typename T>
using §(16_alias_template)alias = T;
}

// TypeAlias template referring to another alias.
namespace alias_chain {
template<typename T>
using A = T;

template<typename T>
using §(17_alias_template_chain)AA = A<T>;
}

// Constant array.
namespace constant_array {
using m_int = int;

m_int §(18_constant_array)arr[10];
}

// Incomplete array.
namespace incomplete_array {
using m_int = int;

extern m_int §(19_incomplete_array)arr[];
}

// Dependent size array.
namespace dependent_array {
using m_int = int;

template<int Size>
struct Test {
  m_int §(20_dependent_size_array)arr[Size];
};
}

// Dependent names resolved through the template resolver.
namespace dependent_names {
template <typename T> struct Base {
  using type = T;
  static constexpr int value = 1;
};

template <typename T> struct Derived : Base<T> {
  typename Base<T>::§(21_dependent_type)type x;
  static constexpr int y = Base<T>::§(22_dependent_value)value;
  using typename Base<T>::§(23_using_typename)type;
  using Base<T>::§(24_using_value)value;
};
}

// Template-template argument.
namespace template_template_arg {
template <typename> struct X {};
template <template <typename> class TT> struct Apply {};
Apply<§(25_template_template_arg)X> a;
}

// CTAD placeholder.
namespace ctad {
template <typename T> struct Box { Box(T); };
§(26_ctad)Box b(1);
}

// Unresolved member overload set with dependent argument.
namespace unresolved_member {
struct A {
  void f(int);
  void f(char);
};
template <typename T> void g(A a, T t) { a.§(27_unresolved_member)f(t); }
}

// sizeof...(pack).
namespace sizeof_pack {
template <class... Ts> constexpr auto n = sizeof...(§(28_sizeof_pack)Ts);
}

// Injected class name.
namespace injected_class_name {
template <class T> struct Node {
  §(29_injected_class_name)Node *next;
};
}

// Dependent member access through the current instantiation.
namespace dependent_member {
template <typename T> struct Base {
  void method();
  int field;
};
template <typename T> struct Derived : Base<T> {
  void f() { this->§(30_dependent_method)method(); }
  int g() { return this->§(31_dependent_field)field; }
};
}

// Dependent lookup through a nondependent base.
namespace fixed_base {
struct Fixed { using type = int; };
template <class T> struct FixedDerived : Fixed {};
template <class T> typename FixedDerived<T>::§(32_fixed_base)type h();
}

// Most specialized partial wins.
namespace partial_order {
template <class A, class B> struct X { static constexpr int value = 0; };
template <class A, class B> struct X<A*, B> { static constexpr int value = 1; };
template <class A> struct X<A*, A*> { static constexpr int value = 2; };
template <class T> int g() { return X<T*, T*>::§(33_partial_order)value; }
}

// Overloaded arrow chain.
namespace arrow_chain {
template <class T> struct Node { void method(); };
template <class T> struct Ptr { Node<T> *operator->(); };
template <class T> void f(Ptr<T> p) { p->§(34_arrow_chain)method(); }
}

// Use site of a dependent using-typename.
namespace unresolved_using_use {
template <class T> struct B2 { using type = T; };
template <class T> struct D2 : B2<T> {
  using typename B2<T>::type;
  §(35_unresolved_using_use)type x;
};
}

// An uninstantiated variable-template specialization also documents itself
// with the pattern instantiation would pick.
namespace var_primary_fallback {
// comment from primary
template <typename T> int slot = 0;
// comment from specialization
template <typename T> int slot<T*> = 1;

using probe = decltype(sl§(36_variable_template_doc)ot<int*>);
}

// A member alias with a dependent underlying type keeps its sugar:
// canonicalizing would spell the parameter as `type-parameter-0-0`.
namespace dependent_member_alias {
template <typename T>
struct Wrap {
  using §(37_dependent_member_alias)element = const T&;
};
}
