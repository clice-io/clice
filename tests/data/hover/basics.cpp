// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Best foo ever.
void fo§(01_global_func)o() {}

namespace ns1 { namespace ns2 {
  /// Best foo ever.
  void fo§(02_ns_func)o() {}
}}

namespace ns1 { namespace ns2 {
  class Foo {
    char b§(03_field)ar;
    double y[2];
  };
}}

union FooUnion {
  char b§(04_union_field)ar;
  double y[2];
};

struct FooBits1 {
  int §(05_bitfield)x : 1;
  int y : 1;
};

struct FooBits2 {
  char x;
  char §(06_bitfield_padding)y : 1;
  int z;
};

namespace ns1 { namespace ns2 {
  struct Bar {
    void foo() {
      int b§(07_method_local)ar;
    }
  };
}}

namespace ns1 { namespace {
  struct {
    char b§(08_anon_struct_field)ar;
  } T;
}}

struct §(09_struct_size)X {};

void pf1() {
  __f§(10_predefined_var)unc__;
}

template<int> void pf2() {
  __f§(11_predefined_var_dependent)unc__;
}
