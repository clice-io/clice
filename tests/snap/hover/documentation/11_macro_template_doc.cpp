/// # Template keyword from a macro
///
/// - status: partial
/// - issues: clangd#1226
///
/// Documentation on a macro-produced template is missing from hover
///
/// When the `template` keyword is produced by a macro expansion, the
/// declaration's doc comment does not appear on hover, and the card carries
/// no description.

int anchor = 0;

#define TEMPLATE template

/// A documented template function.
TEMPLATE <typename T> void §(01_macro_template)run(T value);
