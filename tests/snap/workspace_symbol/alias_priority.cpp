/// # Workspace Symbol
///
/// ## Underlying declarations ranked above type aliases
///
/// - status: unsupported
/// - order: 7
/// - issues: clangd#2253
///
/// When both `ConnectionImpl` and its alias `Connection` match a query,
/// the underlying declaration should rank first. Results carry no
/// ranking today.

// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
