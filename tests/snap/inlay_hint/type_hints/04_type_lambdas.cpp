/// # Lambdas
///
/// - status: supported
/// - issues: clangd#1163
///
/// Variables, deduced return types, and init-captures all hint

int compute();

void use() {
    auto callback = [captured = compute()](int x) {
        return x + captured;
    };
    auto bare = [] {
        return 1.5;
    };
}
