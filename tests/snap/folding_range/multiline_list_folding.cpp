/// # Fold Kinds
///
/// ## Multi-line list folding — function parameters, call arguments, initializer lists, lambda captures
///
/// - status: supported
/// - order: 3

void configure(
    int width,       // ┐
    int height,      // │ foldable parameter list
    bool fullscreen  // ┘
);

int compute(int a, int b, int c);

void demo() {
    int values[] = {
        1,  // ┐
        2,  // │ foldable initializer list
        3   // ┘
    };

    int result = compute(
        values[0],  // ┐
        values[1],  // │ foldable argument list
        values[2]   // ┘
    );

    auto sum = [
        first = values[0],   // ┐
        second = values[1]   // ┘ foldable lambda capture
    ] {
        return first + second;
    };

    auto scale = [](
        int base,    // ┐ foldable lambda
        int factor   // ┘ parameter list
    ) {
        return base * factor;
    };

    result += sum() + scale(result, 2);
}

int accumulate(
    int start,  // ┐
    int step,   // │ foldable parameter list
    int count   // ┘ on a definition
) {
    return start + step * count;
}

void log_all(
    const char* format,  // ┐ variadic parameter
    ...                  // ┘ list still folds
);

struct Rect {
    Rect(int w, int h);
};

Rect area(
    10,  // ┐ foldable constructor
    20   // ┘ arguments
);

Rect brace_area{
    30,
    40
};
