/// # Missing Symbols
///
/// ## Local symbols — variables and types declared inside function bodies nest under their function
///
/// - status: supported
/// - issues: clangd#616
/// - order: 3

int compute() {
    int local_sum = 0;

    struct Accumulator {
        int total;
    };

    auto twice = [](int x) {
        return 2 * x;
    };

    struct Pair {
        int a;
        int b;
    };

    auto [first, second] = Pair{1, 2};

    return local_sum + twice(first) + second;
}
