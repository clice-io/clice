/// # Macros
///
/// ## Macro definition and expansion
///
/// - status: supported
/// - snap: skip
/// - order: 1

// snap: skip because the server compiles the `#define` inside the
// preamble PCH, where SQUARE stays a plain `macro` token; the standalone
// single-pass compile sees definition and expansion together and merges
// them into `conflict`. Un-skip once the two paths agree.
#define SQUARE(x) ((x) * (x))

[[maybe_unused]] static int squared = SQUARE(4);
