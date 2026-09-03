/// # Fuzzy matching — word-boundary-aware scoring for camelCase and snake_case
///
/// - status: unsupported
/// - issues: clangd#914
///
/// Matching is a case-insensitive substring test: `LinLis` does not find
/// `LinkedList`, and `pcfg` does not find `parse_config`. Word-boundary
/// initials should match and score for every symbol kind, macros
/// included.

// query: LinLis
// query: pcfg

struct LinkedList {};

void parse_config();
