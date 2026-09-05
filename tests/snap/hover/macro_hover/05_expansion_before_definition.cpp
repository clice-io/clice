/// # Use before definition
///
/// - status: partial
/// - issues: clangd#2642
///
/// A macro use before its definition has no hover yet
///
/// A macro name used in an `#if` above its own `#define` has no hover, while a
/// use after the definition works normally.

int anchor = 0;

#if §(01_before_def)COUNT > 0
int positive = 1;
#endif

#define COUNT 3

int use = §(02_after_def)COUNT;
