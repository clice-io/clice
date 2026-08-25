/// # Workspace Symbol
///
/// ## Overload disambiguation — parameter types shown in results
///
/// - status: partial
/// - verify: server
/// - order: 3
/// - issues: clangd#1344
///
/// Querying an overloaded name finds every overload, but each entry
/// carries only the bare name — nothing tells the two `process` results
/// apart short of opening both locations.

// query: process

void process(int value) {}

void process(bool flag, int level) {}
