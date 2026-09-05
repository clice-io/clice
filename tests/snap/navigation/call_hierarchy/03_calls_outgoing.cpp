/// # Outgoing calls
///
/// - status: supported
/// - verify: server
///
/// Outgoing calls list every function a body invokes, one entry per
/// callee

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
