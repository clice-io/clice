/// # Functions & Snippets
///
/// ## Snippets defer to bundling — while overloads are bundled, argument snippets stay off even when enabled
///
/// - status: supported
/// - order: 5
/// - config: {"enable_function_arguments_snippet": true}
/// - diagnostics: expected

// The completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);

int z = fo§(pos)
