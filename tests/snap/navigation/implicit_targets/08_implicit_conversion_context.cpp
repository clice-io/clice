/// # Implicit conversion operators
///
/// - status: unsupported
/// - issues: clangd#1931
///
/// From a conversion context to the operator
///
/// Go-to-definition from a context that runs a user-defined conversion (a
/// condition, `!`, an explicit `bool(...)`) should reach the conversion
/// operator; today it returns nothing.

struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if (g) {}      // go-to-def on ( → Guard::operator bool
    bool ok = !g;  // go-to-def on ! → Guard::operator bool
}
