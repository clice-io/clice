/// # Partitions in interface units
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// In an interface unit, only the module's interface partitions complete
///
/// An interface cannot export an internal partition, and names it imports from one may not reach the interface's importers, so internal partitions are offered only to the module's other units.

// snap: Server-only because completion reads the server's module map.
export module app;
import :§(partitions);
