/// # Declarations & References
///
/// ## Structured bindings — binding names at definition and use
///
/// - status: partial
/// - order: 8
///
/// The binding-name token currently leaks onto the opening `[` and merges
/// with the first name, so the definition renders as one `[a` token.

struct Pair {
    int first, second;
};

void unpack() {
    auto §[a, §b] = Pair{1, 2};
    int sum = §a + §b;
}
