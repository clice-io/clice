/// # Deprecated tagging
///
/// - status: supported
/// - diagnostics: expected
///
/// A [[deprecated]] candidate carries the Deprecated tag, its plain sibling does not

// The completion prefix cuts the initializer mid-expression.
[[deprecated]] int old_thing(int x);
int new_thing(int x);

int z = thing§(pos)
