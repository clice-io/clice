/// # Closing delimiter
///
/// - status: supported
/// - verify: server
/// - diagnostics: expected
///
/// Accepting a header closes the directive, replacing a delimiter already
/// typed after the cursor instead of doubling it

// snap: Server-only because include-path completion is answered before compilation.
#include "snap_hea§(open)
#include "snap_hea§(closed)der.h"
