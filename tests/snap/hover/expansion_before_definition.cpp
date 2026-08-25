/// # Macro Hover
///
/// ## Use before definition — hovering a macro name that appears before its `#define`
///
/// - status: partial
/// - order: 5
/// - issues: clangd#2642
///
/// A macro name used in an `#if` above its own `#define` should still hover
/// with the macro's definition. clice currently returns no hover at the
/// pre-definition use; a use after the `#define` works normally.

int anchor = 0;

#if §(01_before_def)COUNT > 0
int positive = 1;
#endif

#define COUNT 3

int use = §(02_after_def)COUNT;
