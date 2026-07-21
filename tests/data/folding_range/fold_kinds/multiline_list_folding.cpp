/// # Multi-line list folding — function parameters, call arguments, initializer lists, lambda captures
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
}
