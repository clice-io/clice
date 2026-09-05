/// # Attribute names
///
/// - status: unsupported
/// - issues: clangd#2209
///
/// Standard and vendor attributes, and expressions inside them

[[nodiscard]] int compute();
[[deprecated("use v2")]] void old_func();
[[maybe_unused]] int counter = 0;

struct [[gnu::packed]] Packed {};
