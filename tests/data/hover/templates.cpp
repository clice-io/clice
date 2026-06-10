// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Variable with template type.
namespace var_tmpl_type {
template <typename T, class... Ts> class Foo { public: Foo(int); };
Foo<int, char, bool> fo$(01_var_of_template_type)o = Foo<int, char, bool>(5);
}

// Implicit template instantiation.
namespace implicit_instantiation {
template <typename T> class vector{};
vec$(02_implicit_instantiation)tor<int> foo;
}

// Class template.
namespace class_template {
template <template<typename, bool...> class C,
          typename = char,
          int = 0,
          bool Q = false,
          class... Ts> class Foo final {};
template <template<typename, bool...> class T>
F$(03_class_template)oo<T> foo;
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
  fo$(04_function_template)o<Foo>();
}
}

// Function decl.
namespace function_decl {
template<typename, bool...> class Foo {};
Foo<bool, true, false> foo(int, bool T = false);

void bar() {
  fo$(05_function_decl)o(3);
}
}

// Partially-specialized class template.
namespace partial_spec {
template <typename T> class X;
template <typename T> class $(06_partial_specialization)X<T*> {};
}

// Constructor of partially-specialized class template.
namespace partial_spec_ctor {
template<typename, typename=void> struct X;
template<typename T> struct X<T*>{ $(07_partial_spec_constructor)X(); };
}

namespace destructor {
class X { $(08_destructor)~X(); };
}

namespace conversion_operator {
class X { op$(09_conversion_operator)erator int(); };
}

namespace conversion_target {
class X { operator $(10_conversion_target)X(); };
}

// Falls back to primary template, when the type is not instantiated.
namespace primary_fallback {
// comment from primary
template <typename T> class Foo {};
// comment from specialization
template <typename T> class Foo<T*> {};
void foo() {
  Fo$(11_primary_template_doc)o<int*> *x = nullptr;
}
}

// Var template decl.
namespace var_template {
using m_int = int;

template <int Size> m_int $(12_variable_template)arr[Size];
}

// Var template decl specialization.
namespace var_template_spec {
using m_int = int;

template <int Size> m_int arr[Size];

template <> m_int $(13_variable_template_spec)arr<4>[4];
}

// Canonical type.
namespace canonical_type {
template<typename T>
struct TestHover {
  using Type = T;
};

void code() {
  TestHover<int>::Type $(14_canonical_type)a;
}
}

// Canonical template type.
namespace canonical_tmpl_type {
template<typename T>
void $(15_function_template_type)foo(T arg) {}
}

// TypeAlias template.
namespace alias_template {
template<typename T>
using $(16_alias_template)alias = T;
}

// TypeAlias template referring to another alias.
namespace alias_chain {
template<typename T>
using A = T;

template<typename T>
using $(17_alias_template_chain)AA = A<T>;
}

// Constant array.
namespace constant_array {
using m_int = int;

m_int $(18_constant_array)arr[10];
}

// Incomplete array.
namespace incomplete_array {
using m_int = int;

extern m_int $(19_incomplete_array)arr[];
}

// Dependent size array.
namespace dependent_array {
using m_int = int;

template<int Size>
struct Test {
  m_int $(20_dependent_size_array)arr[Size];
};
}
