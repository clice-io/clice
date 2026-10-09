/// # Bracket token types
///
/// - status: unsupported
///
/// Matching brackets do not receive pair-specific token kinds yet

template <typename T>
struct Grid {
    T cells[4];
};

Grid<int> grid{{1, 2, 3, 4}};

int first(Grid<int>& grid) {
    return grid.cells[0];
}
