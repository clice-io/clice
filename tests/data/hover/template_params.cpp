// Test cases ported from clangd's HoverTests.cpp (llvmorg-21.1.8), part of the LLVM project,
// licensed under Apache License v2.0 with LLVM Exceptions.

// Template type parameter.
namespace type_param {
template <typename $(01_type_param)T = int> void foo();
}

// Template template parameter.
namespace template_template_param {
template <template<typename> class $(02_template_template_param)T> void foo();
}

// Non-type template parameter.
namespace non_type_param {
template <int $(03_non_type_param)T = 5> void foo();
}
