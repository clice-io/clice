// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Local variable.
namespace local_var {
int main() {
  int bonjour;
  §(01_local_var)bonjour = 2;
  int test1 = bonjour;
}
}

// Local variable in method.
namespace method_var {
struct s {
  void method() {
    int bonjour;
    §(02_method_local_var)bonjour = 2;
  }
};
}

// Global variable
static int hey = 10;
void inc_hey() {
  §(03_global_var)hey++;
}

namespace ns1 {
  static long long hey = -36637162602497;
}
void inc_ns_hey() {
  ns1::§(04_ns_global_var)hey++;
}

namespace ns {
  namespace {
    int foo;
  } // anonymous namespace
} // namespace ns
int main() { ns::§(05_anon_ns_var)foo++; }
