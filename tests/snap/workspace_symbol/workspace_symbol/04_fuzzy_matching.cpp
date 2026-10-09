/// # Fuzzy matching
///
/// - status: supported
/// - issues: clangd#914
/// - verify: server
///
/// A query matches a name as a subsequence aligned to its words
///
/// `LinLis` finds `LinkedList` and `pconf` finds `parse_config`: after its
/// first letter, every letter of the query either continues a run or starts
/// a word of the name, so `pcfg` finds nothing — its `f` lands in the middle
/// of `config`. The name spelled exactly ranks first, then the names
/// starting with the query, then matches deeper inside a name.

// query: LinLis
// query: pconf
// query: pcfg

struct LinkedList {};

void parse_config();
