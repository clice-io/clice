/// # `delete` expression
///
/// - status: unsupported
///
/// `delete` does not navigate to the invoked destructor yet
///
/// Go-to-definition on `delete` does not reach the destructor it runs.

struct Widget {
    ~Widget();
};

void dispose(Widget* widget) {
    delete widget;  // go-to-def on delete → Widget::~Widget
}
