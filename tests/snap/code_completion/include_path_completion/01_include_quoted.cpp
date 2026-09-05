/// # Quoted include paths
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Headers and directories from the configured search path, directories marked by a trailing slash

// snap: Server-only because include-path completion is answered before compilation.
#include "snap§(pos)"
