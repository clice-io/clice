/// # Designator Hints
///
/// ## Parenthesized aggregate initialization — C++20 `Point(1, 2)` gets no hints yet
///
/// - status: unsupported
/// - issues: clangd#2540
/// - order: 7

struct Point {
    int x;
    int y;
};

Point p(1, 2);
