/// # Symbol Kinds
///
/// ## Macros in the preamble region — definitions in the leading directive run outline on the inspect path, while the server's preamble record does not surface them yet
///
/// - status: partial
/// - snap: skip
/// - order: 7

// snap: skip because the server compiles the leading directive run into
// the preamble PCH, whose macro record the live parse does not yet see —
// these definitions outline on the inspect path only. Un-skip once the
// preamble channel serves them.

#define PREAMBLE_LIMIT 8
#define PREAMBLE_CHECK(cond) (!!(cond))

int after = PREAMBLE_LIMIT;
