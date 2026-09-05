/// # Fuzzy matching
///
/// - status: unsupported
/// - issues: clangd#914
///
/// Workspace symbol search does not support word-boundary fuzzy matching yet
///
/// Matching is a case-insensitive substring test: `LinLis` does not find
/// `LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
/// initials do not match for any symbol kind, including macros.

// query: LinLis
// query: pcfg

struct LinkedList {};

void parse_config();
