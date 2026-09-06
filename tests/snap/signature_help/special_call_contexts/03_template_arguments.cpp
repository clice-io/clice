/// # Template argument lists
///
/// - status: supported
/// - issues: clangd#299, clangd#1387
///
/// Template parameters show as the signature; a class template points at its
/// kind, not a return type

template <typename T, typename U>
struct Pair {};

Pair<int, §(pos) double> p;
