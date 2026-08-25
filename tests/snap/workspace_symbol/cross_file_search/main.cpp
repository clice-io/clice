/// # Workspace Symbol
///
/// ## Search spans the whole project — hits from files other than the queried one
///
/// - status: supported
/// - verify: server
/// - indexing: true
/// - order: 2
///
/// The query returns symbols from project files that are not even open
/// in the editor: `other.h` stays closed here, so its hit is served by
/// the background index.

// snap: other.h is a markerless header, so the server driver never opens
// snap: it — the pinned hit can only come from the background index of
// snap: the closed file, not from open-session aggregation.
// indexed: helper_elsewhere

// query: helper_elsewhere

int local_anchor = 0;
