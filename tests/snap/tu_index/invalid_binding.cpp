// - verify: inspect
// - diagnostics: expected
//
// A structured binding that cannot decompose leaves bindings without a
// type; the rest of the file still indexes.

void broken() {
    auto [a, b] = 0;
}

struct Pair {
    int first;
    int second;
};

int fine() {
    auto [left, right] = Pair{};
    return left + right;
}
