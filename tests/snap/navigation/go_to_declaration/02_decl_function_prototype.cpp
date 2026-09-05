/// # Functions
///
/// - status: supported
/// - verify: server
///
/// From a use or out-of-line definition to the prototype
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
