/// # Whitespace and newlines
///
/// - status: partial
/// - issues: clangd#2057
///
/// A markdown table in a comment keeps its line breaks
///
/// A markdown table written across several `///` lines should render as a
/// table with its line breaks preserved. clice currently flattens the lines
/// onto one line, so the table does not render.

namespace tables {
/// | Column A | Column B |
/// |----------|----------|
/// | 1        | 2        |
void §(01_table)table_fn();
}
