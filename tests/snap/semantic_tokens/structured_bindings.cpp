/// # Declarations & References
///
/// ## Structured bindings — binding names at definition and use
///
/// - status: supported
/// - order: 8
///
/// The opening `[` deliberately carries no token; only the binding names
/// themselves are highlighted.

struct Pair {
    int first, second;
};

void unpack() {
    auto §[§a, §b] = Pair{1, 2};
    int sum = §a + §b;
}
