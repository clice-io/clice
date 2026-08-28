/// # Lexical Tokens
///
/// ## Inactive regions at the top of a file — untaken branches among the leading directives dim the same way
///
/// - status: supported
/// - order: 6
/// - snap: separate

// snap: the whole file is preamble up to `int after`; on the server path
// it compiles into the PCH, whose defines have no semantic nodes in the
// main parse — KEEP loses the `definition` modifier there (pre-existing
// preamble gap, unrelated to the inactive tagging this fixture pins).

#define KEEP 1
#if 0
#define DEAD 2
#endif

int after = KEEP;
