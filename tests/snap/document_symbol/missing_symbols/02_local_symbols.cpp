/// # Local symbols
///
/// - status: supported
/// - issues: clangd#616
///
/// Variables and types declared inside function bodies nest under their
/// function

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
