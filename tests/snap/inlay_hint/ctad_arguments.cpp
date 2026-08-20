/// # Other Hint Kinds
///
/// ## CTAD arguments — deduced class template arguments after the template name
///
/// - status: unsupported
/// - issues: clangd#2331
/// - order: 2

template <typename A, typename B>
struct Pair {
    A first;
    B second;
    Pair(A a, B b);
};

// Could hint `<int, double>` after `pair`.
Pair pair(1, 2.5);
