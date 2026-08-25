/// # Documentation
///
/// ## Comment suppression option — a config switch to hide misattributed doc comments
///
/// - status: unsupported
/// - order: 12
/// - issues: clangd#2148
///
/// A stray comment picked up by the association heuristic — a section
/// banner separated from the code by a blank line, for example — always
/// reaches the hover card: clice has no config option to suppress doc
/// comments whose attachment is a guess.

namespace suppression {
// TODO: tidy this file up.

int §(01_misattributed_note)counter;
}
