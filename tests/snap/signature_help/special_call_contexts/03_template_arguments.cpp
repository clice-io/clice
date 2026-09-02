/// # Template argument lists — template parameters show as the signature; a class template points at its kind, not a return type
///
/// - status: supported
/// - issues: clangd#299, clangd#1387

template <typename T, typename U>
struct Pair {};

Pair<int, §(pos) double> p;
