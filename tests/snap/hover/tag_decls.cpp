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

namespace inherited_class {
struct first {};
struct second {};

class Point : public first, virtual protected second {
  int value;
};
void foo() {
  §(18_inherited_class)Point point;
}
}

namespace template_fields {
template <typename T>
struct Box {
  using value_type = T;
  T value;
};
void foo() {
  §(19_template_fields)Box<int> box;
}
}

namespace record_members {
struct Traits {
public:
  static constexpr int extent = 4;
  using value_type = int;
  template <typename T> using pointer = T*;
  enum class Kind { first, second, third, fourth, fifth, sixth, seventh, eighth, ninth };
  struct Node { int hidden; };
  struct Forward;
  template <typename T> struct Nested { T hidden; };

private:
  static int instances;
  int value = [] { return 42; }();
  static int create();
  void reset() {}
};
void foo() {
  §(20_record_members)Traits traits;
}
}

namespace record_member_limit {
struct Values {
  int v01;
  int v02;
  int v03;
  int v04;
  int v05;
  int v06;
  int v07;
  int v08;
  int v09;
  int v10;
  int v11;
  int v12;
  int v13;
  int v14;
  int v15;
  int v16;
  int v17;
  int v18;
  int v19;
  int v20;
  int v21;
};
void foo() {
  §(21_record_member_limit)Values values;
}
}

namespace top_level_enum {
enum Value { v01, v02, v03, v04, v05, v06, v07, v08, v09 };
void foo() {
  §(22_top_level_enum)Value value = v01;
}
}

#define HOVER_REPEAT_4(x) x, x, x, x
#define HOVER_REPEAT_256(x) HOVER_REPEAT_4(HOVER_REPEAT_4(HOVER_REPEAT_4(HOVER_REPEAT_4(x))))

namespace large_record_initializer {
struct Tables {
  int large_field[256] = { HOVER_REPEAT_256(0) };
  inline static int large_static[256] = { HOVER_REPEAT_256(0) };
  int small_field = 1;
  inline static int small_static = 2;
};
void foo() {
  §(23_large_record_initializer)Tables tables;
}
}

#undef HOVER_REPEAT_256
#undef HOVER_REPEAT_4
