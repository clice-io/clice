/// # Go to Definition
///
/// ## Declaration-only symbols navigate to their declaration
///
/// - status: supported
/// - verify: server
/// - order: 3
///
/// Symbols that carry only a declaration — pure virtuals, `extern`
/// variables, out-of-line static constants — resolve to that declaration
/// instead of returning nothing.

extern int threshold;

int probe(int value);

int watch(int value) {
    return §(fn_use)probe(value) + §(var_use)threshold;
}
