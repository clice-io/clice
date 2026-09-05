/// # Numeric literals
///
/// - status: unsupported
/// - issues: clangd#1669
///
/// Type and value of an integer or float literal
///
/// Hovering a numeric literal yields no card, unlike character and string
/// literals, whose type and value are shown.

namespace numeric_literal_type {

auto count = 42;
auto ratio = 3.14;

}
