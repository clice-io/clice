// Names introduced without a type of their own: structured bindings, a
// range-for variable, a lambda capture.

struct Pair {
    int first;
    double second;
};

Pair make();

void bindings() {
    auto [§(binding)first, second] = make();
    (void)§(binding_use)first;
    int items[3]{};
    for (auto& §(range_variable)item : items) {
        (void)item;
    }
    static thread_local auto [§(thread_local_binding)shared, unused] = make();
    (void)shared;
    Pair pairs[2]{};
    for (auto [§(range_binding)key, value] : pairs) {
        (void)key;
    }
    int threshold = 3;
    auto above = [&§(capture)threshold](int v) { return v > §(capture_use)threshold; };
    (void)above;
}
