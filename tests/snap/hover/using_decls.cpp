// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Function definition via using declaration.
namespace using_func {
namespace ns {
  void foo();
}
int main() {
  using ns::foo;
  §(01_using_function)foo();
}
}

// Using declaration with two possible function declarations.
namespace using_overloads {
namespace ns { void foo(int); void foo(char); }
using ns::foo;
template <typename T> void bar() { §(02_using_overloads)foo(T{}); }
}

// Hover at the declaration site of a using declaration.
namespace using_decl_site {
namespace ns { void bar(int); }
using ns::§(03_using_decl_site)bar;
}

// Type imported through a using declaration.
namespace using_type {
struct B { using value_type = int; };
struct D : B {
  using B::value_type;
  §(04_using_type)value_type x;
};
}

// Using directive through a namespace alias.
namespace alias_directive {
namespace original { }
namespace alias = original;
using namespace §(05_alias_directive)alias;
}
