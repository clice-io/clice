/// # Macros in the preamble region
///
/// - status: partial
/// - snap: skip
///
/// Definitions in the leading directive run outline on the inspect path, while the server's preamble record does not surface them yet

// snap: skip because the server compiles the leading directive run into
// snap: the preamble PCH, whose macro record the live parse does not yet see —
// snap: these definitions outline on the inspect path only. Un-skip once the
// snap: preamble channel serves them.

#define PREAMBLE_LIMIT 8
#define PREAMBLE_CHECK(cond) (!!(cond))

int after = PREAMBLE_LIMIT;
