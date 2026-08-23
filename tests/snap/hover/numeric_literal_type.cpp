/// # Expression Context
///
/// ## Numeric literals — type and value of an integer or float literal
///
/// - status: unsupported
/// - issues: clangd#1669
/// - order: 6
///
/// Hovering a numeric literal yields no card, unlike character and string
/// literals, whose type and value are shown.

namespace numeric_literal_type {

auto count = 42;
auto ratio = 3.14;

}
