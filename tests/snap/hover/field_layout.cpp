// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

/// # Layout Information
///
/// ## Field layout — size, offset, alignment and padding show on field hover
///
/// - status: supported
/// - order: 1
///
/// The corpus pins an x86-64 target, so the bit numbers are stable.

struct Header {
    char t§(plain_field)ag;
    int len§(padded_field)gth;
};

struct Flags {
    int rea§(bitfield)dy : 1;
    int e§(bitfield_padding)nd : 1;
};
