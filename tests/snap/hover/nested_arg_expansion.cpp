/// # Macro Hover
///
/// ## Nested macro in arguments — a macro named inside another invocation's arguments
///
/// - status: partial
/// - order: 4
///
/// The recorded expansion starts at the outer invocation, so hovering an
/// inner macro named inside the arguments shows only its definition, not an
/// expansion preview.

int anchor = 0;

#define ECHO(x) x
#define INNER_VAL 99

int nested = ECHO(§(01_nested_arg)INNER_VAL);
