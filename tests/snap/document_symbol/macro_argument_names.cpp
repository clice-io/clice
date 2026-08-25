/// # Location Correctness
///
/// ## Names spelled in macro arguments — the selection range points at the name written in the macro argument; names spelled in the macro body fall back to the invocation site
///
/// - status: supported
/// - issues: clangd#1941
/// - order: 2

// The assertion holds the directives out of the preamble region, whose
// live record the server path does not yet see.
static_assert(true);

#define VAR(X) int X = 1;

VAR(from_argument)

#define COUNTER() int counter_from_body = 0;

COUNTER()
