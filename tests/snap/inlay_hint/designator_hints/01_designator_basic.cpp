/// # Field and index designators
///
/// - status: supported
/// - issues: clangd#2303
///
/// Positional aggregate initialization shows `.field=` and `[index]=`

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
