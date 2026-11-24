struct Base {
    Base(int x);
};

struct Widget : Base {
    Widget(int size);
    Widget() : §(delegating)Widget§(delegating_paren)(42) {}
};

Widget::Widget(int size) : §(base)Base§(base_paren)(size) {}
