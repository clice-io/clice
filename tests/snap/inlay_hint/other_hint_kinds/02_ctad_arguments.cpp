/// # CTAD arguments
///
/// - status: unsupported
/// - issues: clangd#2331
///
/// CTAD does not display deduced class template arguments yet

template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

// Could hint `<int, double>` after `pair`.
Pair pair(1, 2.5);
