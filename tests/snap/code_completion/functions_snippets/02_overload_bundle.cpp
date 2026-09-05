/// # Overload bundling
///
/// - status: supported
/// - diagnostics: expected
///
/// An overload set collapses into one entry with an overload count

// The completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);

int x = fooo§(pos)
