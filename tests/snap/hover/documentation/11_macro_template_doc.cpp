/// # Template keyword from a macro
///
/// - status: partial
/// - issues: clangd#1226
///
/// The docstring should survive the expansion
///
/// When the `template` keyword is produced by a macro expansion, the
/// declaration's doc comment should still appear on hover. clice currently
/// drops it — the card carries no description.

int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void §(01_macro_template)run(T value);
