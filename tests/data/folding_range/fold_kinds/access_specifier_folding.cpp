/// # Access-specifier section folding — `public:` / `protected:` / `private:` regions within a class
///
/// - status: supported
/// - issues: clangd#1455
/// - order: 4

class Widget {
public:            // ┐
    void draw();   // │ foldable
    void resize(); // ┘
private:           // ┐
    int width;     // │ foldable
    int height;    // ┘
};
