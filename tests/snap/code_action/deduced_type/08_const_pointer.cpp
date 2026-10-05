/// # Constant deduced pointers
///
/// - status: supported
///
/// A `const` written before an `auto` that deduced a pointer moves behind the `*`, keeping the pointer itself constant
///
/// When other specifiers stand between the `const` and the `auto`, the declaration is left as written.

void f(int* pointer, int** table) {
    const §(pointer)auto p = pointer;
    const §(pointee)auto* row = table;
    static const §(specifiers)auto s = pointer;
    const static §(parted)auto t = pointer;
}
