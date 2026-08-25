/// # Call Hierarchy
///
/// ## Incoming calls
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// Incoming calls list every caller of a function, and a caller that
/// invokes it more than once contributes each call site.

int §(target)helper(int v) {
    return v;
}

int alpha() {
    return helper(1);
}

int beta() {
    return helper(2) + helper(3);
}
