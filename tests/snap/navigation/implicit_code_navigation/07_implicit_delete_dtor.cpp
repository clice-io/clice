/// # `delete` expression — navigate to the destructor
///
/// - status: unsupported
///
/// Go-to-definition on `delete` should reach the destructor it runs; today
/// it returns nothing.

struct Widget {
    ~Widget();
};

void dispose(Widget* widget) {
    delete widget;  // go-to-def on delete → Widget::~Widget
}
