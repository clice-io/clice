/// # Nested compound-statement folding — `if`/`for`/`while` bodies inside functions
///
/// - status: unsupported
/// - order: 2

void process(int count) {
    if (count > 0) {                       // ┐
        for (int i = 0; i < count; ++i) {  // │ nested blocks that could
            // ... work ...                // │ fold independently of
        }                                  // │ the enclosing function
    }                                      // ┘
}
