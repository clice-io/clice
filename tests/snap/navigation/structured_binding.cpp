/// - verify: server

struct Pair {
    int first;
    int second;
};

Pair make_pair() {
    return {1, 2};
}

int sum_pair() {
    auto [§(left_def)left, §(right_def)right] = make_pair();
    return §(left_use)left + §(right_use)right;
}
