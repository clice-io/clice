/// # `#define` inside the preamble
///
/// - status: unsupported
///
/// Hover on a leading directive
///
/// A `#define` in the leading run of directives before the first declaration
/// has no hover card, while definitions after a declaration do.

#define §(01_preamble_define)EARLY 1

int use = EARLY;
