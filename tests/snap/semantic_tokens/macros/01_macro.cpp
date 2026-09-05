/// # Macro definition and expansion
///
/// - status: supported
/// - snap: skip
///

// snap: skip because the server compiles the `#define` inside the
// snap: preamble PCH, where SQUARE stays a plain `macro` token; the inspect-path
// snap: single-pass compile sees definition and expansion together and merges
// snap: them into `conflict`. Un-skip once the two paths agree.

#define SQUARE(x) ((x) * (x))

[[maybe_unused]] static int squared = SQUARE(4);
