/// # Nested compound-statement folding
///
/// - status: supported
///
/// Nested control-flow bodies form folding ranges

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

    // Each branch folds on its own; the line that closes one branch and
    // opens the next stays visible.
    if (count > 10) {
        count += 1;
    } else {
        count -= 1;
    }

    try {
        count *= 2;
    } catch (...) {
        count = 0;
    }

    do {
        count -= 1;
    } while (count > 10);
}
