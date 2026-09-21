/// # Define all missing members
///
/// - status: supported
/// - issues: clangd#445
///
/// On the class name, every member function without a definition is defined at once, in declaration order
///
/// Members that already have a definition, pure virtuals and defaulted
/// members are left alone.

struct §(all)Widget {
    Widget();
    ~Widget() = default;
    virtual int size() const = 0;
    void reset();
    void grow(int by);
    int id() const { return 0; }
};

void Widget::reset() {
}
