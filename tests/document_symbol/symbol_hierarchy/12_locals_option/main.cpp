struct Accumulator {
    int total;
};

int compute() {
    int local_sum = 0;
    auto twice = [](int x) {
        int doubled = 2 * x;
        return doubled;
    };
    return local_sum + twice(1);
}
