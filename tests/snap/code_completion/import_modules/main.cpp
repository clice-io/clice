/// # Module Completion
///
/// ## Import statements — known module names complete after `import`, with the closing semicolon inserted
///
/// - status: supported
/// - order: 1
/// - verify: server
/// - diagnostics: expected
///
/// Answered by the server from its module map, so only the server path
/// exists for this fixture; the sibling module interface is opened first
/// so the module is known. The statement stays unterminated — a `;` on
/// the line means the import is already complete and nothing is offered.

import ma§(pos)
