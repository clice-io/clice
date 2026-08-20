/// # Fold Kinds
///
/// ## Nested compound-statement folding — `if`/`for`/`while` bodies inside functions
///
/// - status: supported
/// - order: 2

void process(int count) {
    if (count > 0) {
        for (int i = 0; i < count; i += 1) {
            count -= 1;
        }
    }

    while (count > 0) {
        count -= 1;
    }

    // A bare scope block folds too.
    {
        int scratch = count;
        count = scratch + 1;
    }
}
