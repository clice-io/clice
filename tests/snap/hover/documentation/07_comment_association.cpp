/// # Banner comments
///
/// - status: partial
/// - issues: clangd#974
///
/// A separated section banner still attaches to the following declaration
///
/// A `// ==== Section ====` banner followed by a blank line is misattributed
/// as documentation for the declaration below it, so the banner text appears
/// in the card.

namespace banners {
// ==== Section Banner ====

void §(01_after_banner)foo();
}
