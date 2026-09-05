/// # Access-specifier section folding
///
/// - status: supported
/// - issues: clangd#1455
///
/// Access-specifier regions within a class form folding ranges

class Widget {
public:            // ┐
    void draw();   // │ foldable
    void resize(); // ┘
private:           // ┐
    int width;     // │ foldable
    int height;    // ┘
};
