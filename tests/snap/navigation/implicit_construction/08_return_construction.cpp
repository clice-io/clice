/// # Return value implicit construction
///
/// - status: supported
/// - verify: server
///
/// A braced return value navigates to the selected constructor
///
/// A braced `return {args}` implicitly constructs the function's return
/// type; go-to-definition on the brace reaches the selected constructor.

struct Widget {
    Widget(int w, int h);
};

Widget create() {
    return §(ret_brace){800, 600};
}
