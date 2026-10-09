/// # Macros
///
/// - status: supported
/// - config: {"enable_function_arguments_snippet": true}
/// - diagnostics: expected
///
/// Object-like macros complete as constants, function-like ones as functions
/// with a parameter signature; argument snippets follow the function setting

#define RETRY_LIMIT 3

#define CLAMP(value, limit) ((value) < (limit) ? (value) : (limit))

int a = RETRY§(object);
int b = CLA§(function);
