/// # Bracket token types
///
/// - status: unsupported
///
/// Matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds

template <typename T>
struct Grid {
    T cells[4];
};

Grid<int> grid{{1, 2, 3, 4}};

int first(Grid<int>& grid) {
    return grid.cells[0];
}
