struct Widget {
    void §(decl)draw();
};

void Widget::§(def)draw() {}

void render(Widget& widget) {
    widget.§(use)draw();
}
