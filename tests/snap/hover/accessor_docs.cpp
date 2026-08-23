// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Documentation
///
/// ## Synthesized accessor docs — trivial getters/setters get a generated one-line description
///
/// - status: supported
/// - order: 2
///
/// A trivial getter or setter with no comment of its own gets a synthesized
/// "Trivial accessor/setter for `field`." line in its hover card.

namespace accessors {
struct Widget {
    int width;
    int §(01_getter)getWidth() { return width; }
    void §(02_setter)setWidth(int w) { width = w; }
};
}
