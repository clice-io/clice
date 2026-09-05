/// # Structured bindings
///
/// - status: supported
///
/// Each binding hints its canonical type; the aggregate itself stays bare

struct Pair {
    int first;
    float second;
};

Pair make();

int array[2];

void use() {
    auto [a, b] = make();
    auto [x, y] = array;
}
