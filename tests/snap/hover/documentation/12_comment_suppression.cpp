/// # Comment suppression option
///
/// - status: unsupported
/// - issues: clangd#2148
///
/// Misattributed documentation cannot be suppressed by configuration yet
///
/// A stray comment such as a section banner separated from the code by a blank
/// line always reaches the hover card, and no option suppresses it.

namespace suppression {
// TODO: tidy this file up.

int §(01_misattributed_note)counter;
}
