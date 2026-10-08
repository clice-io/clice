/// # Partitions of the current module
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// In a module unit, the module's own partitions complete as `:partition`, while neither the module itself nor the partitions of other modules are offered
///
/// A statement that already ends in a semicolon keeps it.

// snap: Server-only because completion reads the server's module map.
module app;
import §(names);
import :§(partitions);
