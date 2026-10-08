/// # Import statements
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Known module names complete after `import`, with the closing semicolon
/// inserted

// snap: Server-only because completion reads the server's module map; the sibling
// snap: module interface is opened first so that map contains the module.
import ma§(pos)
