/// # Workspace Symbol
///
/// ## Search spans the whole project — hits from files other than the queried one
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// A query made from `main.cpp` also returns symbols defined in the
/// project's other sources.

// query: helper_elsewhere

int local_anchor = 0;
