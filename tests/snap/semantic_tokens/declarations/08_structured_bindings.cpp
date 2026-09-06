/// # Structured bindings
///
/// - status: supported
///
/// Structured binding names receive variable tokens at definition and use
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
