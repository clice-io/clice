/// # Implicit Code Navigation
///
/// ## Lambda init-capture — navigate to the constructor
///
/// - status: unsupported
/// - order: 13
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
