/// # Implicit conversion hints
///
/// - status: unsupported
/// - issues: clangd#2254
///
/// Implicit conversions at call sites have no hints yet

void process(double val);

// Could hint `(double)` before the argument.
void use() {
    process(42);
}
