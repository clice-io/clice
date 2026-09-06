/// # Parenthesized aggregate initialization
///
/// - status: unsupported
/// - issues: clangd#2540
///
/// C++20 `Point(1, 2)` gets no hints yet

struct Point {
    int x;
    int y;
};

Point p(1, 2);
