/// # Implicit Code Navigation
///
/// ## Structured bindings — navigate to the underlying accessors or fields
///
/// - status: unsupported
/// - order: 20
///
/// Go-to-definition on a structured binding name resolves to the binding
/// itself rather than the underlying field or accessor it names.

struct Pair {
    int first;
    int second;
};

void use(Pair p) {
    // go-to-def on a → Pair::first, on b → Pair::second
    auto [a, b] = p;
}
