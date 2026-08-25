/// # Implicit Code Navigation
///
/// ## Return value implicit construction — navigate to the constructor
///
/// - status: supported
/// - verify: server
/// - order: 12
///
/// A braced `return {args}` implicitly constructs the function's return
/// type; go-to-definition on the brace reaches the selected constructor.

struct Widget {
    Widget(int w, int h);
};

Widget create() {
    return §(ret_brace){800, 600};
}
