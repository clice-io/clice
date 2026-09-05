/// # CTAD arguments
///
/// - status: unsupported
/// - issues: clangd#2331
///
/// Deduced class template arguments after the template name

template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

// Could hint `<int, double>` after `pair`.
Pair pair(1, 2.5);
