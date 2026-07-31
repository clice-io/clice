/// # Declarations & References
///
/// ## `sizeof...` — the pack parameter keeps its type-parameter token
///
/// - status: supported
/// - issues: clangd#213
/// - order: 12

template <typename... Ts>
constexpr auto count = sizeof...(§Ts);
