/// # Go to Declaration
///
/// ## Declaration and definition with cosmetically different signatures
///
/// - status: supported
/// - verify: server
/// - order: 7
///
/// Parameter names, and a top-level `const` on a parameter, are not part
/// of a function's type: the declaration and the definition below spell the
/// same function differently, yet go-to-declaration still connects a use to
/// the prototype.

int §(decl)render(int width, const int height);

int §(def)render(int w, int h) {
    return w * h;
}

int use_render() {
    return §(use)render(800, 600);
}
