/// # Alias ranking
///
/// - status: unsupported
/// - issues: clangd#2253
///
/// Underlying declarations should rank above matching aliases
///
/// Results carry no ranking today.

// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
