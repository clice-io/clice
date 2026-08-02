/// # Filtering & Ranking
///
/// ## Deprecated tagging — a [[deprecated]] candidate carries the Deprecated tag, its plain sibling does not
///
/// - status: supported
/// - order: 2

// error-ok: the completion prefix cuts the initializer mid-expression.
[[deprecated]] int old_thing(int x);
int new_thing(int x);

int z = thing§(pos)
