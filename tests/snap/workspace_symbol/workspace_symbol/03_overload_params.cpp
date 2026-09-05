/// # Overload disambiguation
///
/// - status: partial
/// - issues: clangd#1344
/// - verify: server
///
/// Parameter types shown in results
///
/// Querying an overloaded name finds every overload, but each entry
/// carries only the bare name — nothing tells the two `process` results
/// apart short of opening both locations.

// query: process

void process(int value) {}

void process(bool flag, int level) {}
