/// # Type Hints
///
/// ## Explicitly spelled initializers — casts and functional casts still hint redundantly
///
/// - status: partial
/// - issues: clangd#1749
/// - order: 8

int compute();

void use() {
    // The type is already written on the right-hand side; ideally these
    // two hints would be suppressed.
    auto widened = static_cast<long>(compute());
    auto braced = int{42};
}
