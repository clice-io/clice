/// # Template parameter hints
///
/// - status: unsupported
/// - issues: clangd#2583
///
/// Template argument hints are not emitted at call sites yet

template <typename T, typename U>
T convert(U val);

// Could hint `T: float` next to the explicit argument list.
float converted = convert<float>(42);
