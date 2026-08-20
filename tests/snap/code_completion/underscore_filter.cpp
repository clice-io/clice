/// # Filtering & Ranking
///
/// ## Underscore filtering — underscore-prefixed internal symbols hide unless the typed prefix itself starts with one
///
/// - status: supported
/// - order: 1
/// - diagnostics: expected

// The completion prefixes are undeclared identifiers. The
// statements stay semicolon-terminated: an unterminated one puts the
// NEXT marker into a recovery context, which completion drops entirely.
int _private_thing;
int public_thing;

int x = pu§(hidden);
int y = _p§(typed_underscore);
