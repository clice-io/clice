/// # Whitespace and newlines
///
/// - status: partial
/// - issues: clangd#2057
///
/// Markdown tables in comments lose their line breaks on hover
///
/// A markdown table written across several `///` lines is flattened onto one
/// line, so the table does not render.

namespace tables {
/// | Column A | Column B |
/// |----------|----------|
/// | 1        | 2        |
void §(01_table)table_fn();
}
