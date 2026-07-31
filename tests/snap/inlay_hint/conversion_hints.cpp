/// # Other Hint Kinds
///
/// ## Implicit conversion hints — surface the conversions a call site performs
///
/// - status: unsupported
/// - issues: clangd#2254
/// - order: 3

void process(double val);

// Could hint `(double)` before the argument.
void use() {
    process(42);
}
