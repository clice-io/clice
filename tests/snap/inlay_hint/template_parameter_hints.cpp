/// # Other Hint Kinds
///
/// ## Template parameter hints — deduced and explicit template arguments at call sites
///
/// - status: unsupported
/// - issues: clangd#2583
/// - order: 1

template <typename T, typename U>
T convert(U val);

// Could hint `T: float` next to the explicit argument list.
float converted = convert<float>(42);
