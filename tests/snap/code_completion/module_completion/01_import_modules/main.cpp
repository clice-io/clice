/// # Import statements
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Known module names complete after `import`, with the closing semicolon
/// inserted
///
/// A statement that already contains its closing semicolon is complete and
/// offers no module names.

// snap: Server-only because completion reads the server's module map; the sibling
// snap: module interface is opened first so that map contains the module.
import ma§(pos)
