/// # Access specifier grouping — `public:` / `private:` / `protected:` as grouping nodes for breadcrumb navigation
///
/// - status: unsupported
/// - issues: clangd#499

class Widget {
public:
    void draw();
    void resize();

private:
    int width;
    int height;
};
