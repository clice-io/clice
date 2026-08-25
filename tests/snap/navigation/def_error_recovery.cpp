/// # Go to Definition
///
/// ## Error recovery — navigate to a variable whose type is unresolved
///
/// - status: unsupported
/// - order: 9
///
/// When a variable's type name fails to resolve, go-to-definition on a
/// later use of the variable currently returns nothing, even though the
/// variable's own declaration is still recorded.

Unresolved handle;  // 'Unresolved' does not name a type

void read() {
    (void) handle;  // go-to-def on handle → the declaration above
}
