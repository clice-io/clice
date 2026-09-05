/// # Implicit conversion hints
///
/// - status: unsupported
/// - issues: clangd#2254
///
/// Surface the conversions a call site performs

void process(double val);

// Could hint `(double)` before the argument.
void use() {
    process(42);
}
