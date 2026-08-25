/// # Workspace Symbol
///
/// ## Enumerator lookup under the enum's scope
///
/// - status: unsupported
/// - order: 6
/// - issues: clangd#931
///
/// `Color::Red` should find the enumerator — for scoped and unscoped
/// enums alike — but qualified queries match nothing; only the bare
/// `Red` does.

// query: Color::Red

enum Color { Red, Green };
