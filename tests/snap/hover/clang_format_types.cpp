/// # Type Information
///
/// ## Type formatting — clang-format applied to rendered types
///
/// - status: unsupported
/// - issues: clangd#2156
/// - order: 11
///
/// Long or nested types are printed by the compiler's default type printer;
/// they are not re-wrapped or aligned through clang-format.

namespace clang_format_types {

template <typename A, typename B, typename C, typename D>
struct Tuple {};

Tuple<int, long, unsigned, char> wide;

}
