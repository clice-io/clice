// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.
// Every point in this file is expected to produce NO hover.

$(01_builtin_type)int nh_func1() {}

void nh_func2() {$(02_empty_braces)}

// FIXME: "decltype(auto)" should be a single hover
decltype(au$(03_decltype_auto_inner)to) nh_x1 = 0;

// FIXME: not supported yet
// Lambda auto parameter
auto nh_lamb = [](a$(04_lambda_auto_param)uto){};

// non-named decls don't get hover. Don't crash!
$(05_static_assert)static_assert(1, "");

// non-evaluatable expr
template <typename T> void nh_func3() {
  (void)size$(06_dependent_sizeof)of(T);
}

// literals
auto nh_a = t$(07_bool_literal)rue;
auto nh_b = $(08_compound_literal)(int){42};
auto nh_c = $(09_float_literal)42.;
auto nh_d = $(10_imaginary_literal)42.0i;
auto nh_e = $(11_int_literal)42;
auto nh_f = $(12_nullptr_literal)nullptr;
