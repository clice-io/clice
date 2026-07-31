/// # Token Modifiers
///
/// ## User-defined operators — distinguish overloaded operators from built-in ones
///
/// - status: unsupported
/// - issues: clangd#1521
/// - order: 10

struct Vec {
    Vec operator+(const Vec& other) const;
};

Vec add(Vec a, Vec b) {
    return a + b;
}

int add(int a, int b) {
    return a + b;
}
