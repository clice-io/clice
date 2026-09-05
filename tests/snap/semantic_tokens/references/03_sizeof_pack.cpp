/// # `sizeof...`
///
/// - status: supported
/// - issues: clangd#213
///
/// The pack parameter keeps its type-parameter token

template <typename... Ts>
constexpr auto count = sizeof...(§Ts);
