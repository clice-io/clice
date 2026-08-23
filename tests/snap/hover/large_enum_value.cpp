/// # Hover Correctness
///
/// ## Large unsigned enum constant — hovering a `0xFFFF...ULL` enumerator does not crash
///
/// - status: supported
/// - order: 3
///
/// clangd crashes on this (clangd#2381); clice renders the full unsigned
/// value without overflow.

namespace big_enum {

enum class Flags : unsigned long long {
    Ma§(max_value)x = 0xFFFFFFFFFFFFFFFFULL,
};

}
