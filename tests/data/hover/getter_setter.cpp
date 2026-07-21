// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

namespace std { template<typename T> T&& move(T&& t); }

// Trivial getter.
namespace getter {
struct X { int Y; float §(01_getter)y() { return Y; } };
}

// Trivial setter.
namespace setter {
struct X { int Y; void §(02_setter)setY(float v) { Y = v; } };
}

// Trivial setter returning *this.
namespace setter_builder {
struct X { int Y; X& §(03_setter_builder)setY(float v) { Y = v; return *this; } };
}

// Trivial setter using std::move.
namespace setter_move {
struct X { int Y; void §(04_setter_move)setY(float v) { Y = std::move(v); } };
}
