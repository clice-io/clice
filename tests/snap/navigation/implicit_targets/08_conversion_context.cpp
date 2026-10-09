/// # Implicit conversion operators
///
/// - status: unsupported
/// - issues: clangd#1931
///
/// Conversion contexts do not navigate to the invoked conversion operator yet
///
/// Go-to-definition from a context that runs a user-defined conversion (a
/// condition, `!`, an explicit `bool(...)`) does not reach the conversion
/// operator.

struct Guard {
    explicit operator bool() const;
};

void use(Guard g) {
    if (g) {}      // go-to-def on ( → Guard::operator bool
    bool ok = !g;  // go-to-def on ! → Guard::operator bool
}
