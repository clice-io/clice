/// # Unbundled overloads — with bundling off, every overload is its own entry with its own signature
///
/// - status: supported
/// - config: {"bundle_overloads": false}
/// - diagnostics: expected

// The completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);

int x = fooo§(pos)
