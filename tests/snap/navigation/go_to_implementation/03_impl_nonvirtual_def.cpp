/// # Non-virtual function
///
/// - status: unsupported
/// - issues: clangd#854
///
/// Declaration to out-of-line definition
///
/// Go-to-implementation on a non-virtual function declaration should reach
/// its out-of-line definition, behaving as a superset of go-to-definition;
/// today it returns nothing.

struct Widget {
    void draw();  // go-to-impl on draw → out-of-line definition below
};

void Widget::draw() {}
