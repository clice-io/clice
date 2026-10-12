// The explicit object parameter never hints (C++23).
struct Widget {
    void resize(this Widget& self, int width, int height);
};

void use() {
    Widget w;
    w.resize(800, 600);
}
