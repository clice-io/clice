/// # Include Path Completion
///
/// ## Quoted include paths — headers and directories from the configured search path, directories marked by a trailing slash
///
/// - status: supported
/// - order: 1
/// - verify: server
/// - diagnostics: expected
///
/// Answered by the server before any compilation, so only the server path
/// exists for this fixture.

#include "snap§(pos)"
