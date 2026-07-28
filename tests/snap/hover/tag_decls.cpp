// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Struct.
namespace struct_decl {
namespace ns1 {
  struct MyClass {};
} // namespace ns1
int main() {
  ns1::§(01_struct)MyClass* Params;
}
}

// Class.
namespace class_decl {
namespace ns1 {
  class MyClass {};
} // namespace ns1
int main() {
  ns1::§(02_class)MyClass* Params;
}
}

// Union.
namespace union_decl {
namespace ns1 {
  union MyUnion { int x; int y; };
} // namespace ns1
int main() {
  ns1::§(03_union)MyUnion Params;
}
}

namespace forward_decl {
// Forward class declaration
class Foo;
class Foo {};
§(04_forward_class)Foo* foo();
}

namespace enum_def {
// Enum declaration
enum Hello {
  ONE, TWO, THREE,
};
void foo() {
  §(05_enum)Hello hello = ONE;
}
}

// Enumerator.
namespace enumerator {
enum Hello {
  ONE, TWO, THREE,
};
void foo() {
  Hello hello = §(06_enumerator)ONE;
}
}

// C++20's using enum.
namespace using_enum {
enum class Hello {
  ONE, TWO, THREE,
};
void foo() {
  using enum Hello;
  Hello hello = §(07_using_enum)ONE;
}
}

// Enumerator in anonymous enum.
namespace anon_enum {
enum {
  ONE, TWO, THREE,
};
void foo() {
  int hello = §(08_anon_enumerator)ONE;
}
}

namespace typedef_decl {
// Typedef
typedef int Foo;
int main() {
  §(09_typedef)Foo bar;
}
}

namespace typedef_embedded {
// Typedef with embedded definition
typedef struct Bar {} Foo;
int main() {
  §(10_typedef_embedded)Foo bar;
}
}

// Namespace.
namespace ns_decl {
namespace ns {
struct Foo { static void bar(); };
} // namespace ns
int main() { §(11_namespace)ns::Foo::bar(); }
}

// Field in anonymous struct.
namespace anon_struct {
static struct {
  int hello;
} s;
void foo() {
  s.§(12_anon_struct_field)hello++;
}
}

// Anonymous union.
namespace anon_union {
struct outer {
  union {
    int abc, def;
  } v;
};
void g() { struct outer o; o.v.§(13_anon_union_field)def++; }
}

namespace templated_fn {
// Templated function
template <typename T>
T foo() {
  return 17;
}
void g() { auto x = §(14_templated_function)foo<int>(); }
}

// Should not crash on dependent method call.
namespace method_no_crash {
template <class T> struct cls {
  int method();
};

auto test = cls<int>().§(15_template_method)method();
}

// Type of nested templates: the variable.
namespace nested_templates_var {
template <class T> struct cls {};
cls<cls<cls<int>>> fo§(16_nested_template_var)o;
}

namespace nested_templates_class {
// type of nested templates.
template <class T> struct cls {};
§(17_nested_template_class)cls<cls<cls<int>>> foo;
}
