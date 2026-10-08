/// # Macro arguments
///
/// - status: supported
/// - issues: clangd#1324
///
/// A name or expression written in a macro argument is selected where it is
/// written, then the argument list and the whole invocation
///
/// Code a macro's replacement spells around the argument has no place in
/// the file of its own: the steps go from the argument straight to the
/// invocation.

#define DECLARE(name) int name = 0
#define TWICE(value) ((value) * 2)

DECLARE(§(declared)counter);

int doubled() {
    return TWICE(§(expr)counter + 1);
}
