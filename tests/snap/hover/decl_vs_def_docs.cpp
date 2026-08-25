/// # Documentation
///
/// ## Declaration vs definition comments — the declaration's doc wins over a definition-site comment
///
/// - status: supported
/// - order: 8
///
/// clangd tracks this as clangd#829; clice already prefers the
/// declaration's `///` documentation over the definition's plain `//` note,
/// showing it at both the declaration and the definition site.

namespace decldef {
/// Public API documentation.
void §(01_at_decl)process(int x);

// Internal implementation note.
void §(02_at_def)process(int x) { (void)x; }
}
