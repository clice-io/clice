/// # Documentation
///
/// ## `@copydoc` tags — copy another symbol's documentation onto this one
///
/// - status: partial
/// - order: 3
/// - issues: clangd#1320
///
/// A `@copydoc target` tag should copy `target`'s documentation into this
/// symbol's hover card. clice does not resolve the tag yet — the card shows
/// the literal `@copydoc base_func()` text.

namespace copydoc {
/// Detailed documentation.
void §(01_base)base_func();

/// @copydoc base_func()
void §(02_wrapper)wrapper();
}
