/// # Attributes
///
/// ## Attribute names — standard and vendor attributes, and expressions inside them
///
/// - status: unsupported
/// - issues: clangd#2209
/// - order: 1

[[nodiscard]] int compute();
[[deprecated("use v2")]] void old_func();
[[maybe_unused]] int counter = 0;

struct [[gnu::packed]] Packed {};
