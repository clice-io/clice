/// # Call Hierarchy
///
/// ## Outgoing calls
///
/// - status: supported
/// - verify: server
/// - order: 3
///
/// Outgoing calls list every function a body invokes, one entry per
/// callee.

int one() {
    return 1;
}

int two() {
    return 2;
}

int three() {
    return 3;
}

int §(caller)dispatch() {
    return one() + two() + three();
}
