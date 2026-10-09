/// # Variadic signature
///
/// - status: supported
/// - diagnostics: expected
///
/// A trailing `...` shows in the parameter detail

// The completion prefix cuts the initializer mid-expression.
int printf_like(const char* fmt, ...);

int x = printf§(pos)
