/// # Nested macro in arguments
///
/// - status: partial
///
/// A nested macro argument shows its definition without an expansion preview
///
/// The expansion preview starts at the outer invocation, so hovering an
/// inner macro named inside the arguments shows only its definition, not an
/// expansion preview.

int anchor = 0;

#define ECHO(x) x
#define INNER_VAL 99

int nested = ECHO(§(01_nested_arg)INNER_VAL);
