/// # Access specifier grouping
///
/// - status: unsupported
/// - issues: clangd#499
///
/// `public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation

class Widget {
public:
    void draw();
    void resize();

private:
    int width;
    int height;
};
