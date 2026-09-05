/// # Template parameter hints
///
/// - status: unsupported
/// - issues: clangd#2583
///
/// Deduced and explicit template arguments at call sites

template <typename T, typename U>
T convert(U val);

// Could hint `T: float` next to the explicit argument list.
float converted = convert<float>(42);
