/// # Snippets defer to bundling
///
/// - status: supported
/// - config: {"enable_function_arguments_snippet": true}
/// - diagnostics: expected
///
/// While overloads are bundled, argument snippets stay off even when enabled

// The completion prefix cuts the initializer mid-expression.
int foooo(int x);
int foooo(int x, int y);

int z = fo§(pos)
