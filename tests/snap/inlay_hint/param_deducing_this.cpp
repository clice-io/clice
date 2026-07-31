/// # Parameter Hints
///
/// ## Deducing `this` — the explicit object parameter never hints (C++23)
///
/// - status: supported
/// - issues: clangd#1777
/// - order: 8

struct Widget {
    void resize(this Widget& self, int width, int height);
};

void use() {
    Widget w;
    w.resize(800, 600);
}
