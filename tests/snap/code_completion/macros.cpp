/// # Symbols
///
/// ## Macros — object-like macros complete as constants, function-like ones as functions with a parameter signature; argument snippets follow the function setting
///
/// - status: supported
/// - order: 5
/// - config: {"enable_function_arguments_snippet": true}
/// - diagnostics: expected

#define RETRY_LIMIT 3

#define CLAMP(value, limit) ((value) < (limit) ? (value) : (limit))

int a = RETRY§(object);
int b = CLA§(function);
