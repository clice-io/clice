/// # Declaration vs definition comments
///
/// - status: supported
///
/// The declaration's doc wins over a definition-site comment
///
/// The declaration's `///` documentation appears at both the declaration and
/// definition sites instead of the definition's plain `//` note.

namespace decldef {
/// Public API documentation.
void §(01_at_decl)process(int x);

// Internal implementation note.
void §(02_at_def)process(int x) { (void)x; }
}
