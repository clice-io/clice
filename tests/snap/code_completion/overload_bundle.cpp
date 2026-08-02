/// # Functions & Snippets
///
/// ## Overload bundling — an overload set collapses into one entry with an overload count
///
/// - status: supported
/// - order: 2

// error-ok: the completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);
double foooo(double d);

int x = fooo§(pos)
