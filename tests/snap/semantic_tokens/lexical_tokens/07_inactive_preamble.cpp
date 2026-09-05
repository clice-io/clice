/// # Inactive regions at the top of a file
///
/// - status: supported
/// - snap: separate
///
/// Untaken branches among the leading directives dim the same way

// snap: the whole file is preamble up to `int after`; on the server path
// snap: it compiles into the PCH, whose defines have no semantic nodes in the
// snap: main parse — KEEP loses the `definition` modifier there (pre-existing
// snap: preamble gap, unrelated to the inactive tagging this fixture pins).

#define KEEP 1
#if 0
#define DEAD 2
#endif

int after = KEEP;
