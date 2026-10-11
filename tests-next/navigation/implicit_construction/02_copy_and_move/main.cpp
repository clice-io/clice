struct Widget {
    Widget(int v);
    Widget(const Widget& other);
    Widget(Widget&& other);
    Widget& operator=(const Widget& other);
};

void copies(Widget a) {
    Widget b §(copy_eq)= a;
    Widget c §(move_eq)= static_cast<Widget&&>(a);
    b §(assign_eq)= c;
}
