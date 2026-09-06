/// # Type formatting
///
/// - status: unsupported
/// - issues: clangd#2156
///
/// Long and nested rendered types are not rewrapped or aligned
///
/// Long or nested types are not rewrapped or aligned.

namespace clang_format_types {

template <typename A, typename B, typename C, typename D>
struct Tuple {};

Tuple<int, long, unsigned, char> wide;

}
