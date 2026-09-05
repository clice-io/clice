/// # Access-specifier section folding
///
/// - status: supported
/// - issues: clangd#1455
///
/// `public:` / `protected:` / `private:` regions within a class

class Widget {
public:            // ┐
    void draw();   // │ foldable
    void resize(); // ┘
private:           // ┐
    int width;     // │ foldable
    int height;    // ┘
};
