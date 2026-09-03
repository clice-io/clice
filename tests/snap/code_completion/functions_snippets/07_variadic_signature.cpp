/// # Variadic signature — a trailing `...` shows in the parameter detail
///
/// - status: supported
/// - diagnostics: expected

// The completion prefix cuts the initializer mid-expression.
int printf_like(const char* fmt, ...);

int x = printf§(pos)
