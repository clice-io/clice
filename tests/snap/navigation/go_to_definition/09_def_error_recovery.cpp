/// # Error recovery
///
/// - status: unsupported
///
/// An unresolved variable type prevents navigation to the variable's
/// declaration
///
/// When a variable's type name fails to resolve, go-to-definition on a
/// later use of the variable currently returns nothing, even though the
/// variable's own declaration is still recorded.

Unresolved handle;  // 'Unresolved' does not name a type

void read() {
    (void) handle;  // go-to-def on handle → the declaration above
}
