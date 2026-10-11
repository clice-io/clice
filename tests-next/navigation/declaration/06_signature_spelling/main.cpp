int §(decl)render(int width, const int height);

int §(def)render(int w, int h) {
    return w * h;
}

int use_render() {
    return §(use)render(800, 600);
}
