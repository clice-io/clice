/// # Lexical Tokens
///
/// ## Bracket token types — matching `()`, `[]`, `{}`, `<>` pairs as distinct kinds
///
/// - status: unsupported
/// - order: 10

template <typename T>
struct Grid {
    T cells[4];
};

Grid<int> grid{{1, 2, 3, 4}};
