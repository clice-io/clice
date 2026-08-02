/// # Functions & Snippets
///
/// ## Variadic signature — a trailing `...` shows in the parameter detail
///
/// - status: supported
/// - order: 11

// error-ok: the completion prefix cuts the initializer mid-expression.
int printf_like(const char* fmt, ...);

int x = printf§(pos)
