/// # Declarations & References
///
/// ## `sizeof...` — the pack parameter highlighted as a template parameter
///
/// - status: supported
/// - issues: clangd#213
/// - order: 12

template <typename... Ts>
constexpr auto count = sizeof...(§Ts);
