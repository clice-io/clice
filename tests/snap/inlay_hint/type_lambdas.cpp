/// # Type Hints
///
/// ## Lambdas — variables, deduced return types, and init-captures all hint
///
/// - status: supported
/// - issues: clangd#1163
/// - order: 4

int compute();

void use() {
    auto callback = [captured = compute()](int x) {
        return x + captured;
    };
    auto bare = [] {
        return 1.5;
    };
}
