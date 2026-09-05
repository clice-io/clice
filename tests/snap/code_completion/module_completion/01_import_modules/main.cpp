/// # Import statements
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Known module names complete after `import`, with the closing semicolon inserted
///
/// Answered by the server from its module map, so only the server path
/// exists for this fixture; the sibling module interface is opened first
/// so the module is known. The statement stays unterminated — a `;` on
/// the line means the import is already complete and nothing is offered.

import ma§(pos)
