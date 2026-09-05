/// # User-defined operators
///
/// - status: unsupported
/// - issues: clangd#1521
///
/// Overloaded operators do not differ from built-in operators yet

struct Vec {
    Vec operator+(const Vec& other) const;
};

Vec add(Vec a, Vec b) {
    return a + b;
}

int add(int a, int b) {
    return a + b;
}
