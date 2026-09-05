/// # Non-virtual function
///
/// - status: unsupported
/// - issues: clangd#854
///
/// Non-virtual declarations do not navigate to out-of-line definitions yet
///
/// Go-to-implementation on a non-virtual function declaration does not reach
/// its out-of-line definition and returns nothing.

struct Widget {
    void draw();  // go-to-impl on draw → out-of-line definition below
};

void Widget::draw() {}
