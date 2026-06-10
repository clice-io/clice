// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Constexpr function call value.
namespace constexpr_call {
constexpr int add(int a, int b) { return a + b; }
int b$(01_constexpr_call_value)ar = add(1, 2);
}

// sizeof value.
namespace sizeof_value {
int b$(02_sizeof_value)ar = sizeof(char);
}

// Template member value.
namespace template_member {
template<int a, int b> struct Add {
  static constexpr int result = a + b;
};
int ba$(03_template_member_value)r = Add<1, 2>::result;
}

// Enumerator value.
namespace enumerator_value {
enum Color { RED = -123, GREEN = 5, };
Color x = GR$(04_enumerator_value)EEN;
}

// Variable initialized from an enumerator: symbolic value.
namespace enum_var_value {
enum Color { RED = -123, GREEN = 5, };
Color x = RED;
Color y = $(05_enum_var_value)x;
}

// Static member constant value.
namespace static_member {
template<int a, int b> struct Add {
  static constexpr int result = a + b;
};
int bar = Add<1, 2>::resu$(06_static_member_value)lt;
}

// Constexpr function with aliased return type.
namespace aliased_return {
using my_int = int;
constexpr my_int answer() { return 40 + 2; }
int x = ans$(07_constexpr_fn_value)wer();
}

// String pointer value.
namespace string_pointer {
const char *ba$(08_string_pointer_value)r = "1234";
}

// Should not crash on dependent constructor argument.
namespace dependent_ctor_arg {
template <typename T>
struct Tmpl {
  Tmpl(int name);
};

template <typename A>
void boom(int name) {
  new Tmpl<A>(na$(09_dependent_ctor_arg)me);
}
}

// Should not print inline or anon namespaces.
namespace ns {
  inline namespace in_ns {
    namespace a {
      namespace {
        namespace b {
          inline namespace in_ns2 {
            class Foo {};
          } // in_ns2
        } // b
      } // anon
    } // a
  } // in_ns
} // ns
void foo() {
  ns::a::b::F$(10_skip_inline_anon_ns)oo x;
  (void)x;
}

// auto deduced to instantiation of a template with incomplete argument.
namespace auto_incomplete_arg {
template <typename T> class Foo {};
class X;
void foo() {
  $(11_auto_incomplete_arg)auto x = Foo<X>();
}
}
