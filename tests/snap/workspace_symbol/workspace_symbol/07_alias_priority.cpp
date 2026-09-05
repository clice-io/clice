/// # Alias ranking
///
/// - status: unsupported
/// - issues: clangd#2253
///
/// Matching aliases and underlying declarations have no ranking yet
///
/// Results carry no ranking today.

// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
