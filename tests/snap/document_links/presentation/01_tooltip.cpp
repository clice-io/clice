/// # Resolved-path tooltips
///
/// - status: supported
///
/// Every link carries its target's absolute path as the hover tooltip
///
/// Editors render the tooltip next to the follow-link hint, e.g.
/// `/usr/include/c++/14/vector (ctrl + click)`.

// snap: Link snapshots record only targets; the suite separately compares every
// snap: server tooltip with its target path.
#include "header_a.h"
