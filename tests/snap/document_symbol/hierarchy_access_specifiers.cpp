/// # Symbol Hierarchy
///
/// ## Access specifier grouping — `public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation
///
/// - status: unsupported
/// - issues: clangd#499
/// - order: 3

class Widget {
public:
    void draw();
    void resize();

private:
    int width;
    int height;
};
