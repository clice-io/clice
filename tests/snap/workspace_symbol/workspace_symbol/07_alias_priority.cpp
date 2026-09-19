/// # Alias ranking
///
/// - status: supported
/// - issues: clangd#2253
/// - verify: server
///
/// The name spelled exactly ranks above the names merely starting with it
///
/// `Connection` lists the alias first and `ConnectionImpl` after it.

// query: Connection

struct ConnectionImpl {};

using Connection = ConnectionImpl;
