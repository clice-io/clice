/// # Designator Hints
///
/// ## Field and index designators — positional aggregate initialization shows `.field=` and `[index]=`
///
/// - status: supported
/// - issues: clangd#2303
/// - order: 1

struct Point {
    int x;
    int y;
    int z;
};

Point p{1, 2 + 2};

int coordinates[2] = {7, 8};

// Array designators survive dependent-sized members; reserved names are
// skipped rather than printed.
template <typename T, int N>
struct Array {
    T __elements[N];
};

Array<int, 2> pair = {0, 1};
