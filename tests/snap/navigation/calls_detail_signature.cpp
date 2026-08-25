/// # Call Hierarchy
///
/// ## Function signature in the item detail
///
/// - status: unsupported
/// - order: 4
///
/// A call hierarchy item carries only its name; the function signature is
/// not attached in a detail field, so overloads are indistinguishable in
/// the hierarchy.

int compute(int a, int b) {  // no signature attached to this item
    return a + b;
}

int caller() {
    return compute(1, 2);
}
