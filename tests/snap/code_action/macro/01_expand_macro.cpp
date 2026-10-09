/// # Expand a macro invocation
///
/// - status: supported
/// - issues: clangd#820
///
/// A macro invocation is replaced by the tokens it expands to
///
/// Arguments are substituted; the action is offered from the macro name
/// or anywhere inside its arguments.

#define SQUARE(x) ((x) * (x))
#define LIMIT 42

int f(int n) {
    return §(name)SQUARE(n + §(argument)1) + §(object)LIMIT;
}
