/// # Resolved-path tooltips
///
/// - status: supported
///
/// Every link carries its target's absolute path as the hover tooltip
///
/// Editors render the tooltip next to the follow-link hint, e.g.
/// `/usr/include/c++/14/vector (ctrl + click)`. Snapshots pin only the
/// link targets; the suite instead validates the tooltip against the
/// target on the server reply of every fixture in this corpus.

#include "header_a.h"
