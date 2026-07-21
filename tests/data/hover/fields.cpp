// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Field access.
namespace field_access {
struct Foo { int x; };
int main() {
  Foo bar;
  (void)bar.§(01_field)x;
}
}

// Field with initialization.
namespace field_init {
struct Foo { int x = 5; };
int main() {
  Foo bar;
  (void)bar.§(02_field_init)x;
}
}

// Static field.
namespace static_field {
struct Foo { static int x; };
int main() {
  (void)Foo::§(03_static_field)x;
}
}

// Field in member initializer.
namespace member_initializer {
struct Foo {
  int x;
  Foo() : §(04_member_initializer)x(0) {}
};
}

// Field, GNU old-style field designator.
namespace gnu_designator {
struct Foo { int x; };
int main() {
  Foo bar = { §(05_gnu_designator)x : 1 };
}
}

// Field, field designator.
namespace field_designator {
struct Foo { int x; int y; };
int main() {
  Foo bar = { .§(06_field_designator)x = 2, .y = 2 };
}
}

// Method call.
namespace method_call {
struct Foo { int x(); };
int main() {
  Foo bar;
  bar.§(07_method_call)x();
}
}

// Static method call.
namespace static_method_call {
struct Foo { static int x(); };
int main() {
  Foo::§(08_static_method_call)x();
}
}
