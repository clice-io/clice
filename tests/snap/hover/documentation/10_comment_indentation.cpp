/// # Comment indentation
///
/// - status: partial
/// - issues: clangd#1040
///
/// Indented documentation blocks lose their leading indentation on hover
///
/// A doc comment whose body contains an indented block loses the block's
/// offset, and its blank line collapses.

namespace indented {
/// Summary line.
///
///     step_one();
///     step_two();
void §(01_indented)run();
}
