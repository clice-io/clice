int compute();

void use() {
    auto callback = [captured = compute()](int x) {
        return x + captured;
    };
    auto bare = [] {
        return 1.5;
    };
}
