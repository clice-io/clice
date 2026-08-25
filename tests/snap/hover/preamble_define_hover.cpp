/// # Macro Hover
///
/// ## `#define` inside the preamble — hover on a leading directive
///
/// - status: unsupported
/// - order: 6
///
/// A `#define` in the file's preamble region (the leading run of directives
/// before the first declaration) is not part of the live parse's
/// preprocessor record, so hovering its name yields nothing. Every other
/// macro fixture opens with a declaration precisely to push its directives
/// past the preamble boundary.

#define §(01_preamble_define)EARLY 1

int use = EARLY;
