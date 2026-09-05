/// # Local symbol navigation
///
/// - status: supported
/// - verify: server
///
/// Go-to-definition on a local variable or parameter jumps to its
/// declaration inside the function body

int accumulate(int base) {
    int total = base;
    total = §(local_use)total + §(param_use)base;
    return total;
}
