/// # Go to Definition
///
/// ## Local variables and parameters navigate to their declaration
///
/// - status: supported
/// - verify: server
/// - order: 5
///
/// Go-to-definition on a local variable or parameter jumps to its
/// declaration inside the function body.

int accumulate(int base) {
    int total = base;
    total = §(local_use)total + §(param_use)base;
    return total;
}
