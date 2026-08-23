/// # Documentation
///
/// ## Template keyword from a macro — the docstring should survive the expansion
///
/// - status: partial
/// - order: 11
/// - issues: clangd#1226
///
/// When the `template` keyword is produced by a macro expansion, the
/// declaration's doc comment should still appear on hover. clice currently
/// drops it — the card carries no description.

int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void §(01_macro_template)run(T value);
