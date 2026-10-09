/// # Scoped enumerator lookup
///
/// - status: supported
/// - issues: clangd#931
/// - verify: server
///
/// An enum qualifies its enumerators like any other container

// query: Color::Red

enum Color { Red, Green };
