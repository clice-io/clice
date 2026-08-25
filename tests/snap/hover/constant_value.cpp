// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Expression Context
///
/// ## Constant evaluation — constexpr, enumerators, sizeof
///
/// - status: supported
/// - order: 1
///
/// When an initializer is a constant expression, the card evaluates it and
/// shows the resulting value.

namespace constant_value {

constexpr int square(int n) { return n * n; }
int §(01_constexpr_call)from_call = square(5);

int §(02_sizeof)from_sizeof = sizeof(int);

enum Color { Red = -1, Green = 5 };
Color picked = §(03_enumerator)Green;

template <int A, int B> struct Sum { static constexpr int value = A + B; };
int §(04_static_member)from_member = Sum<3, 4>::value;

}
