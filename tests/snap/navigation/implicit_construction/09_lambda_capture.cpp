/// # Lambda init-capture
///
/// - status: unsupported
///
/// A lambda init-capture does not navigate to its move constructor yet
///
/// Go-to-definition on the `=` of a lambda init-capture does not reach the
/// constructor that builds the captured value.

struct Widget {
    Widget(int v);
    Widget(Widget&& other);
};

void use(Widget w) {
    // go-to-def on = → Widget(Widget&&)
    auto f = [x = static_cast<Widget&&>(w)] {};
}
