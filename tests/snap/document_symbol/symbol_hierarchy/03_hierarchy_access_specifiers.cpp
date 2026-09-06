/// # Access specifier grouping
///
/// - status: unsupported
/// - issues: clangd#499
///
/// Access specifiers do not form grouping nodes in the outline yet

class Widget {
public:
    void draw();
    void resize();

private:
    int width;
    int height;
};
