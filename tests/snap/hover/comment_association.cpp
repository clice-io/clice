/// # Documentation
///
/// ## Banner comments — a section banner separated by a blank line must not attach to the next declaration
///
/// - status: partial
/// - order: 7
/// - issues: clangd#974
///
/// A `// ==== Section ====` banner followed by a blank line should not be
/// misattributed as documentation for the declaration below it. clice
/// currently attaches it anyway — the banner text appears in the card.

namespace banners {
// ==== Section Banner ====

void §(01_after_banner)foo();
}
