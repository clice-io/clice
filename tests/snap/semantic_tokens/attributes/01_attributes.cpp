/// # Attribute names
///
/// - status: unsupported
/// - issues: clangd#2209
///
/// Attribute names and their expressions do not receive semantic tokens yet

[[nodiscard]] int compute();
[[deprecated("use v2")]] void old_func();
[[maybe_unused]] int counter = 0;

struct [[gnu::packed]] Packed {};
