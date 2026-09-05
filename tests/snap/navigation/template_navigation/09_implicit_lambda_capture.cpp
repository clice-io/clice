/// # Lambda init-capture
///
/// - status: unsupported
///
/// Navigate to the constructor
///
/// Go-to-definition on the `=` of a lambda init-capture should reach the
/// constructor that builds the captured value; today it returns nothing.

struct Widget {
    Widget(int v);
    Widget(Widget&& other);
};

void use(Widget w) {
    // go-to-def on = → Widget(Widget&&)
    auto f = [x = static_cast<Widget&&>(w)] {};
}
