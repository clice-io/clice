/// # Structured bindings — each binding hints its canonical type; the aggregate itself stays bare
///
/// - status: supported

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
