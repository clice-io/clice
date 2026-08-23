/// # Documentation
///
/// ## Comment indentation — indented lines in a comment render without spurious extra indentation
///
/// - status: partial
/// - order: 10
/// - issues: clangd#1040
///
/// A doc comment whose body contains an indented block should render with
/// correct indentation. clice currently strips the leading indentation, so
/// an indented code block loses its offset and the blank line collapses.

namespace indented {
/// Summary line.
///
///     step_one();
///     step_two();
void §(01_indented)run();
}
