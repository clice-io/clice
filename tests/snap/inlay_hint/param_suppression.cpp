/// # Parameter Hints
///
/// ## Hint suppression — arguments that already spell the parameter name, and `/*name=*/` comments
///
/// - status: supported
/// - issues: clangd#1877
/// - order: 2

void draw(int width, int height);

void use() {
    int width = 5;
    int h = 2;
    // `width` matches the parameter spelling: only `height:` hints.
    draw(width, h);
    // An inline comment naming the parameter serves the same purpose;
    // a comment naming something else does not.
    draw(/*width=*/1, /*height=*/2);
    draw(/*margin=*/6, 7);
}

struct Sizes {
    static int width;
    int height;

    void member() {
        // A bare member access spells the parameter name: suppressed.
        draw(5, height);
    }
};

void qualified(Sizes s) {
    // A qualified name is not a plain spelling match.
    draw(Sizes::width, 3);
    // Neither is an access through a written base object.
    draw(4, s.height);
}
