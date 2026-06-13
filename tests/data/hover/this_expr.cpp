// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// this expr.
namespace ns {
  class Foo1 {
    Foo1* bar() {
      return t$(01_this_class)his;
    }
  };
}

// this expr for template class.
namespace ns {
  template <typename T>
  class Foo2 {
    Foo2* bar() const {
      return t$(02_this_template_class)his;
    }
  };
}

// this expr for specialization class.
namespace ns {
  template <typename T> class Foo3 {};
  template <>
  struct Foo3<int> {
    Foo3* bar() {
      return thi$(03_this_specialization)s;
    }
  };
}

// this expr for partial specialization struct.
namespace ns {
  template <typename T, typename F> struct Foo4 {};
  template <typename F>
  struct Foo4<int, F> {
    Foo4* bar() const {
      return thi$(04_this_partial_specialization)s;
    }
  };
}
