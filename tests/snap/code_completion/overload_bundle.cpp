/// # Functions & Snippets
///
/// ## Overload bundling — an overload set collapses into one entry with an overload count
///
/// - status: supported
/// - order: 2
/// - diagnostics: expected

// The completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);

int x = fooo§(pos)
