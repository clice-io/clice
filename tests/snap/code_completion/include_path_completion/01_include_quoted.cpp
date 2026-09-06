/// # Quoted include paths
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Completion lists headers and directories from the configured search path and
/// marks directories with a trailing slash

// snap: Server-only because include-path completion is answered before compilation.
#include "snap§(pos)"
