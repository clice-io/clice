/// # `@copydoc` tags
///
/// - status: partial
/// - issues: clangd#1320
///
/// Copied documentation is not resolved onto the receiving symbol yet
///
/// A `@copydoc target` tag remains literal instead of copying `target`'s
/// documentation into this symbol's hover card.

namespace copydoc {
/// Detailed documentation.
void §(01_base)base_func();

/// @copydoc base_func()
void §(02_wrapper)wrapper();
}
