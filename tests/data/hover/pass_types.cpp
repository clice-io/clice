// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

class Base {};
class Derived : public Base {};
class CustomClass {
 public:
  CustomClass() {}
  CustomClass(const Base &x) {}
  CustomClass(int &x) {}
  CustomClass(float x) {}
  CustomClass(int x, int y) {}
};

void int_by_ref(int &x) {}
void int_by_const_ref(const int &x) {}
void int_by_value(int x) {}
void base_by_ref(Base &x) {}
void base_by_const_ref(const Base &x) {}
void base_by_value(Base x) {}
void float_by_value(float x) {}
void custom_by_value(CustomClass x) {}

void fun() {
  int int_x;
  int &int_ref = int_x;
  const int &int_const_ref = int_x;
  Base base;
  const Base &base_const_ref = base;
  Derived derived;
  float float_x;

  // Integer tests
  int_by_value(§(01_int_value_var)int_x);
  int_by_value(§(02_int_value_literal)123);
  int_by_ref(§(03_int_ref_var)int_x);
  int_by_const_ref(§(04_int_const_ref_var)int_x);
  int_by_const_ref(§(05_int_const_ref_literal)123);
  int_by_value(§(06_int_value_from_ref)int_ref);
  int_by_const_ref(§(07_int_const_ref_from_ref)int_ref);
  int_by_const_ref(§(08_int_const_ref_from_const_ref)int_const_ref);
  // Custom class tests
  base_by_ref(§(09_base_ref)base);
  base_by_const_ref(§(10_base_const_ref)base);
  base_by_const_ref(§(11_base_const_ref_from_const_ref)base_const_ref);
  base_by_value(§(12_base_value)base);
  base_by_value(§(13_base_value_from_const_ref)base_const_ref);
  base_by_ref(§(14_derived_to_base_ref)derived);
  base_by_const_ref(§(15_derived_to_base_const_ref)derived);
  base_by_value(§(16_derived_to_base_value)derived);
  // Custom class constructor tests
  CustomClass c1(§(17_ctor_base_const_ref)base);
  auto c2 = new CustomClass(§(18_new_ctor_base_const_ref)base);
  CustomClass c3(§(19_ctor_int_ref)int_x);
  CustomClass c4(int_x, §(20_ctor_int_value)int_x);
  // Converted tests
  float_by_value(§(21_converted_int_var)int_x);
  float_by_value(§(22_converted_int_ref)int_ref);
  float_by_value(§(23_converted_int_const_ref)int_const_ref);
  float_by_value(§(24_float_literal)123.0f);
  float_by_value(§(25_converted_int_literal)123);
  custom_by_value(§(26_converted_custom_from_int)int_x);
  custom_by_value(§(27_converted_custom_from_float)float_x);
  custom_by_value(§(28_converted_custom_from_base)base);
}
