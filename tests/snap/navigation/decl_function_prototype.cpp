/// # Go to Declaration
///
/// ## Functions — from a use or out-of-line definition to the prototype
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// Go-to-declaration reaches a function's prototype both from a call site
/// and from the out-of-line definition — the two non-cursor sites the
/// prototype alternates with.

struct Widget {
    void §(decl)draw();
};

void Widget::§(def)draw() {}

void render(Widget& widget) {
    widget.§(use)draw();
}
