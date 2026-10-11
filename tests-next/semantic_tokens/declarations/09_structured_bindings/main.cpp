struct Pair {
    int first, second;
};

void unpack() {
    auto §(bracket)[§(a_def)a, §(b_def)b] = Pair{1, 2};
    int sum = §(a_use)a + §(b_use)b;
}
