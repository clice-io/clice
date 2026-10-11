struct Widget {
    Widget(int v);
    Widget(Widget&& other);
};

void use(Widget w) {
    auto f = [x §(capture)= static_cast<Widget&&>(w)] {};
}
