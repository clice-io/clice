/// # Workspace Symbol
///
/// ## Search spans the whole project — hits from files other than the queried one
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// The search space is the project, not one buffer: a query typed while
/// editing `main.cpp` still surfaces symbols defined in other sources.

// query: helper_elsewhere

int local_anchor = 0;
